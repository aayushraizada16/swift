//===--- Compilation.cpp - Compilation Task Data Structure ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Driver/Compilation.h"

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/Basic/Program.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/DependencyGraph.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/Driver/ParseableOutput.h"
#include "swift/Driver/Tool.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::sys;
using namespace swift::driver;
using namespace llvm::opt;

Compilation::Compilation(const Driver &D, const ToolChain &DefaultToolChain,
                         DiagnosticEngine &Diags, OutputLevel Level,
                         std::unique_ptr<InputArgList> InputArgs,
                         std::unique_ptr<DerivedArgList> TranslatedArgs,
                         unsigned NumberOfParallelCommands,
                         bool SkipTaskExecution)
  : TheDriver(D), DefaultToolChain(DefaultToolChain), Diags(Diags),
    Level(Level), Jobs(new JobList), InputArgs(std::move(InputArgs)),
    TranslatedArgs(std::move(TranslatedArgs)),
    NumberOfParallelCommands(NumberOfParallelCommands),
    SkipTaskExecution(SkipTaskExecution) {
};

using CommandSet = llvm::DenseSet<const Job *>;

struct Compilation::PerformJobsState {
  /// All jobs which have been scheduled for execution (whether or not
  /// they've finished execution), or which have been determined that they
  /// don't need to run.
  CommandSet ScheduledCommands;

  /// All jobs which have finished execution or which have been determined
  /// that they don't need to run.
  CommandSet FinishedCommands;

  /// A map from a Job to the commands it is known to be blocking.
  ///
  /// The blocked jobs should be scheduled as soon as possible.
  llvm::DenseMap<const Job *, TinyPtrVector<const Job *>> BlockingCommands;
};

Compilation::~Compilation() = default;

void Compilation::addJob(Job *J) {
  Jobs->addJob(J);
}

static const Job *findUnfinishedJob(const JobList &JL,
                                    const CommandSet &FinishedCommands) {
  for (const Job *Cmd : JL) {
    if (!FinishedCommands.count(Cmd))
      return Cmd;
  }
  return nullptr;
}

int Compilation::performJobsInList(const JobList &JL, PerformJobsState &State) {
  // Create a TaskQueue for execution.
  std::unique_ptr<TaskQueue> TQ;
  if (SkipTaskExecution)
    TQ.reset(new DummyTaskQueue(NumberOfParallelCommands));
  else
    TQ.reset(new TaskQueue(NumberOfParallelCommands));

  DependencyGraph<const Job *> DepGraph;
  SmallPtrSet<const Job *, 16> DeferredCommands;
  bool NeedToRunEverything = false;

  // Set up scheduleCommandIfNecessaryAndPossible.
  // This will only schedule the given command if it has not been scheduled
  // and if all of its inputs are in FinishedCommands.
  auto scheduleCommandIfNecessaryAndPossible = [&] (const Job *Cmd) {
    if (State.ScheduledCommands.count(Cmd))
      return;

    if (auto Blocking = findUnfinishedJob(Cmd->getInputs(),
                                          State.FinishedCommands)) {
      State.BlockingCommands[Blocking].push_back(Cmd);
      return;
    }

    State.ScheduledCommands.insert(Cmd);
    TQ->addTask(Cmd->getExecutable(), Cmd->getArguments(), llvm::None,
                (void *)Cmd);
  };

  // Perform all inputs to the Jobs in our JobList, and schedule any commands
  // which we know need to execute.
  for (const Job *Cmd : JL) {
    int res = performJobsInList(Cmd->getInputs(), State);
    if (res != 0)
      return res;

    if (NeedToRunEverything) {
      scheduleCommandIfNecessaryAndPossible(Cmd);
      continue;
    }

    // Try to load the dependencies file for this job. If there isn't one, we
    // always have to run the job, but it doesn't affect any other jobs. If
    // there should be one but it's not present or can't be loaded, we have to
    // run all the jobs.
    Job::Condition Condition = Job::Condition::Always;
    StringRef DependenciesFile =
      Cmd->getOutput().getAdditionalOutputForType(types::TY_SwiftDeps);
    if (!DependenciesFile.empty()) {
      switch (DepGraph.loadFromPath(Cmd, DependenciesFile)) {
      case DependencyGraphImpl::LoadResult::HadError:
        NeedToRunEverything = true;
        break;
      case DependencyGraphImpl::LoadResult::Valid:
        Condition = Cmd->getCondition();
        break;
      case DependencyGraphImpl::LoadResult::NeedsRebuilding:
        llvm_unreachable("we haven't marked anything in this graph yet");
      }
    }

    switch (Condition) {
    case Job::Condition::Always:
      scheduleCommandIfNecessaryAndPossible(Cmd);
      if (!NeedToRunEverything && !DependenciesFile.empty())
        DepGraph.markIntransitive(Cmd);
      break;
    case Job::Condition::CheckDependencies:
      DeferredCommands.insert(Cmd);
      break;
    }
  }

  if (NeedToRunEverything) {
    for (const Job *Cmd : DeferredCommands)
      scheduleCommandIfNecessaryAndPossible(Cmd);
    DeferredCommands.clear();
  }

  int Result = 0;

  // Set up a callback which will be called immediately after a task has
  // started. This callback may be used to provide output indicating that the
  // task began.
  auto taskBegan = [this] (ProcessId Pid, void *Context) {
    // TODO: properly handle task began.
    const Job *BeganCmd = (const Job *)Context;

    // For verbose output, print out each command as it begins execution.
    if (Level == OutputLevel::Verbose)
      BeganCmd->printCommandLine(llvm::errs());
    else if (Level == OutputLevel::Parseable)
      parseable_output::emitBeganMessage(llvm::errs(), *BeganCmd, Pid);
  };

  // Set up a callback which will be called immediately after a task has
  // finished execution. This callback should determine if execution should
  // continue (if execution should stop, this callback should return true), and
  // it should also schedule any additional commands which we now know need
  // to run.
  auto taskFinished = [&] (ProcessId Pid, int ReturnCode, StringRef Output,
                           void *Context) -> TaskFinishedResponse {
    const Job *FinishedCmd = (const Job *)Context;

    if (Level == OutputLevel::Parseable) {
      // Parseable output was requested.
      parseable_output::emitFinishedMessage(llvm::errs(), *FinishedCmd, Pid,
                                            ReturnCode, Output);
    } else {
      // Otherwise, send the buffered output to stderr, though only if we
      // support getting buffered output.
      if (TaskQueue::supportsBufferingOutput())
        llvm::errs() << Output;
    }

    if (ReturnCode != 0) {
      // The task failed, so return true without performing any further
      // dependency analysis.

      // Store this task's ReturnCode as our Result if we haven't stored
      // anything yet.
      if (Result == 0)
        Result = ReturnCode;

      if (!FinishedCmd->getCreator().hasGoodDiagnostics() || ReturnCode != 1)
        Diags.diagnose(SourceLoc(), diag::error_command_failed,
                       FinishedCmd->getCreator().getNameForDiagnostics(),
                       ReturnCode);

      return TaskFinishedResponse::StopExecution;
    }

    // When a task finishes, we need to reevaluate the other commands in our
    // JobList.

    State.FinishedCommands.insert(FinishedCmd);

    auto BlockedIter = State.BlockingCommands.find(FinishedCmd);
    if (BlockedIter != State.BlockingCommands.end()) {
      for (auto *Blocked : BlockedIter->second)
        scheduleCommandIfNecessaryAndPossible(Blocked);
      State.BlockingCommands.erase(BlockedIter);
    }

    // In order to handle both old dependencies that have disappeared and new
    // dependencies that have arisen, we need to reload the dependency file.
    if (!NeedToRunEverything) {
      const CommandOutput &Output = FinishedCmd->getOutput();
      StringRef DependenciesFile =
        Output.getAdditionalOutputForType(types::TY_SwiftDeps);
      if (!DependenciesFile.empty()) {
        SmallVector<const Job *, 16> Dependents;
        bool wasNonPrivate = DepGraph.isMarked(FinishedCmd);

        switch (DepGraph.loadFromPath(FinishedCmd, DependenciesFile)) {
        case DependencyGraphImpl::LoadResult::HadError:
          NeedToRunEverything = true;
          for (const Job *Cmd : DeferredCommands)
            scheduleCommandIfNecessaryAndPossible(Cmd);
          DeferredCommands.clear();
          Dependents.clear();
          break;
        case DependencyGraphImpl::LoadResult::NeedsRebuilding:
          llvm_unreachable("currently unused");
        case DependencyGraphImpl::LoadResult::Valid:
          if (wasNonPrivate)
            DepGraph.markTransitive(Dependents, FinishedCmd);
          break;
        }

        for (const Job *Cmd : Dependents) {
          DeferredCommands.erase(Cmd);
          scheduleCommandIfNecessaryAndPossible(Cmd);
        }
      }
    }

    return TaskFinishedResponse::ContinueExecution;
  };

  auto taskSignalled = [&] (ProcessId Pid, StringRef ErrorMsg, StringRef Output,
                            void *Context) -> TaskFinishedResponse {
    const Job *SignalledCmd = (const Job *)Context;

    if (Level == OutputLevel::Parseable) {
      // Parseable output was requested.
      parseable_output::emitSignalledMessage(llvm::errs(), *SignalledCmd, Pid,
                                             ErrorMsg, Output);
    } else {
      // Otherwise, send the buffered output to stderr, though only if we
      // support getting buffered output.
      if (TaskQueue::supportsBufferingOutput())
        llvm::errs() << Output;
    }

    if (!ErrorMsg.empty())
      Diags.diagnose(SourceLoc(), diag::error_unable_to_execute_command,
                     ErrorMsg);

    Diags.diagnose(SourceLoc(), diag::error_command_signalled,
                   SignalledCmd->getCreator().getNameForDiagnostics());

    // Since the task signalled, so unconditionally set result to -2.
    Result = -2;

    return TaskFinishedResponse::StopExecution;
  };

  // Ask the TaskQueue to execute.
  TQ->execute(taskBegan, taskFinished, taskSignalled);

  // Mark all remaining deferred commands as skipped.
  for (const Job *Cmd : DeferredCommands) {
    if (Level == OutputLevel::Parseable) {
      // Provide output indicating this command was skipped if parseable output
      // was requested.
      parseable_output::emitSkippedMessage(llvm::errs(), *Cmd);
    }

    State.ScheduledCommands.insert(Cmd);
    State.FinishedCommands.insert(Cmd);
  };

  if (Result == 0) {
    assert(State.BlockingCommands.empty() &&
           "some blocking commands never finished properly");
  }

  return Result;
}

static const Job *getOnlyCommandInList(const JobList *List) {
  if (List->size() != 1)
    return nullptr;

  const Job *Cmd = List->front();
  if (Cmd->getInputs().empty())
    return Cmd;
  return nullptr;
}

int Compilation::performSingleCommand(const Job *Cmd) {
  assert(Cmd->getInputs().empty() &&
         "This can only be used to run a single command with no inputs");

  switch (Cmd->getCondition()) {
  case Job::Condition::CheckDependencies:
    return 0;
  case Job::Condition::Always:
    break;
  }

  if (Level == OutputLevel::Verbose)
    Cmd->printCommandLine(llvm::errs());

  SmallVector<const char *, 128> Argv;
  Argv.push_back(Cmd->getExecutable());
  Argv.append(Cmd->getArguments().begin(), Cmd->getArguments().end());
  Argv.push_back(0);

  const char *ExecPath = Cmd->getExecutable();
  const char **argv = Argv.data();

  return ExecuteInPlace(ExecPath, argv);
}

int Compilation::performJobs() {
  // We require buffered output if Parseable output was requested.
  bool RequiresBufferedOutput = (Level == OutputLevel::Parseable);
  if (!RequiresBufferedOutput) {
    if (const Job *OnlyCmd = getOnlyCommandInList(Jobs.get()))
      return performSingleCommand(OnlyCmd);
  }

  if (!TaskQueue::supportsParallelExecution() && NumberOfParallelCommands > 1) {
    Diags.diagnose(SourceLoc(), diag::warning_parallel_execution_not_supported);
  }

  PerformJobsState State;
  int result = performJobsInList(*Jobs, State);

  // FIXME: Do we want to be deleting temporaries even when a child process
  // crashes?
  for (auto &path : TempFilePaths) {
    // Ignore the error code for removing temporary files.
    (void)llvm::sys::fs::remove(path);
  }

  return result;
}
