/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Tries to reduce the input wasm into the smallest possible wasm
// that still generates the same result on a given command. This is
// useful to reduce bug testcases, for example, if a file crashes
// the optimizer, you can reduce it to find the smallest file that
// also crashes it (which generally will show the same bug, in a
// much more debuggable manner).
//

#include <memory>
#include <cstdio>
#include <cstdlib>

#include "pass.h"
#include "support/command-line.h"
#include "support/file.h"
#include "wasm-io.h"
#include "wasm-builder.h"
#include "ast/literal-utils.h"
#include "wasm-printing.h"

using namespace wasm;

// after seeing these destructive reductions, skip and go back to fast pass reduction
static const int MIN_DESTRUCTIVE_REDUCTIONS_TO_STOP = 100;

static void canonicalize(std::string input, std::string output) {
  // reading and writing may alter the size
  int counter = 0;
  while (1) {
    auto oldSize = file_size(input);
    auto code = system(("bin/wasm-opt --vacuum " + input + " -o " + output).c_str());
    assert(code == 0);
    auto newSize = file_size(output);
    if (newSize <= oldSize) break;
    std::cout << "|  <canonicalization: " << oldSize << " => " << newSize << ">\n";
    // do more work, now the input is the output
    input = output;
    counter++;
    assert(counter < 100);
  }
}

struct ProgramResult {
  int code;
  std::string stdout;

  ProgramResult() {}
  ProgramResult(std::string command) {
    getFromExecution(command);
  }

  // runs the command and notes the output
  void getFromExecution(std::string command) {
    // do this using just core stdio.h and stdlib.h, for portability
    // sadly this requires two invokes
    code = system(("timeout 2s " + command + " > /dev/null 2> /dev/null").c_str());
    const int MAX_BUFFER = 1024;
    char buffer[MAX_BUFFER];
    FILE *stream = popen(("timeout 2s " + command + " 2> /dev/null").c_str(), "r");
    while (fgets(buffer, MAX_BUFFER, stream) != NULL) {
      stdout.append(buffer);
    }
    pclose(stream);
  }

  bool operator==(ProgramResult& other) {
    return code == other.code && stdout == other.stdout;
  }
  bool operator!=(ProgramResult& other) {
    return !(*this == other);
  }

  bool failed() {
    return code != 0;
  }

  void dump() {
    std::cout << "[ProgramResult] code: " << code << " stdout: \n" << stdout << "[/ProgramResult]\n";
  }
};

ProgramResult expected;

struct Reducer : public WalkerPass<PostWalker<Reducer>> {
  std::string command, test, working;

  // test is the file we write to that the command will operate on
  // working is the current temporary state, the reduction so far
  Reducer(std::string command, std::string test, std::string working) : command(command), test(test), working(working) {}

  // runs passes in order to reduce, until we can't reduce any more
  // the criterion here is wasm binary size
  void reduceUsingPasses() {
    // run optimization passes until we can't shrink it any more
    std::vector<std::string> passes = {
      "-Oz",
      "-Os",
      "-O1",
      "-O2",
      "-O3",
      "--coalesce-locals",
      "--dce",
      "--duplicate-function-elimination",
      "--inlining-optimizing",
      "--local-cse",
      "--memory-packing",
      "--remove-unused-names --merge-blocks",
      "--optimize-instructions",
      "--precompute",
      "--remove-imports",
      "--remove-memory",
      "--remove-unused-names --remove-unused-brs",
      "--remove-unused-module-elements",
      "--reorder-functions",
      "--reorder-locals",
      "--simplify-locals",
      "--simplify-locals-notee",
      "--simplify-locals-nostructure",
      "--simplify-locals-notee-nostructure",
      "--vacuum"
    };
    auto oldSize = file_size(working);
    bool more = true;
    while (more) {
      //std::cout << "|    starting passes loop iteration\n";
      more = false;
      for (auto pass : passes) {
        auto currCommand = "bin/wasm-opt " + working + " -o " + test + " " + pass;
        //std::cout << "|    trying pass command: " << currCommand << "\n";
        if (!ProgramResult(currCommand).failed()) {
          canonicalize(test, test);
          auto newSize = file_size(test);
          if (newSize < oldSize) {
            // the pass didn't fail, and the size looks smaller, so promising
            // see if it is still has the property we are preserving
            if (ProgramResult(command) == expected) {
              std::cout << "|    command \"" << currCommand << "\" succeeded, reduced size to " << newSize << ", and preserved the property\n";
              copy_file(test, working);
              more = true;
              oldSize = newSize;
            }
          }
        }
      }
    }
  }

  // does one pass of slow and destructive reduction. returns whether it
  // succeeded or not
  // the criterion here is a logical change in the program. this may actually
  // increase wasm size in some cases, but it should allow more reduction later.
  bool reduceDestructively() {
    Module wasm;
    ModuleReader reader;
    reader.read(working, wasm);
    reduced = 0;
    builder = make_unique<Builder>(wasm);
    funcsSeen = 0;
    // before we do any changes, it should be valid to write out the module:
    // size should be as expected, and output should be as expected
    setModule(&wasm);
    assert(writeAndTestReduction());
    // destroy!
    walkModule(&wasm);
    return reduced;
  }

  // destructive reduction state

  Index reduced;
  Expression* beforeReduction;
  std::unique_ptr<Builder> builder;
  Index funcsSeen;

  // write the module and see if the command still fails on it as expected
  bool writeAndTestReduction() {
    ProgramResult result;
    return writeAndTestReduction(result);
  }

  bool writeAndTestReduction(ProgramResult& out) {
    // write the module out
    ModuleWriter writer;
    writer.setBinary(true);
    writer.write(*getModule(), test);
    canonicalize(test, test);
    if (file_size(test) > file_size(working)) {
      //std::cout << "|    sad sizes " << file_size(test) << " !>! " << file_size(working) << '\n';
      return false;
    }
    // test it
    out.getFromExecution(command);
    return out == expected;
  }

  // tests a reduction on the current traversal node, and undos if it failed
  bool tryToReplaceCurrent(Expression* with) {
    if (reduced >= MIN_DESTRUCTIVE_REDUCTIONS_TO_STOP) return false;
    auto* curr = getCurrent();
    //std::cout << "try " << curr << " => " << with << '\n';
    if (curr->type != with->type) return false;
    replaceCurrent(with);
    if (!writeAndTestReduction()) {
      replaceCurrent(curr);
      return false;
    }
    std::cout << "|      tryToReplaceCurrent succeeded (in " << getLocation() << ")\n";
    reduced++;
    copy_file(test, working);
    return true;
  }

  // tests a reduction on an arbitrary child
  bool tryToReplaceChild(Expression*& child, Expression* with) {
    if (reduced >= MIN_DESTRUCTIVE_REDUCTIONS_TO_STOP) return false;
    if (child->type != with->type) return false;
    auto* before = child;
    child = with;
    if (!writeAndTestReduction()) {
      child = before;
      return false;
    }
    std::cout << "|      tryToReplaceChild succeeded (in " << getLocation() << ")\n";
    //std::cout << "|      " << before << " => " << with << '\n';
    reduced++;
    copy_file(test, working);
    return true;
  }

  std::string getLocation() {
    if (getFunction()) return getFunction()->name.str;
    return "(non-function context)";
  }

  // visitors. in each we try to remove code in a destructive and nontrivial way.
  // "nontrivial" means something that optimization passes can't achieve, since we
  // don't need to duplicate work that they do

  void visitBlock(Block* curr) {
    // try to replace each none element with a nop
    Nop nop;
    for (auto*& child : curr->list) {
      if (child->is<Nop>()) continue;
      if (tryToReplaceChild(child, &nop)) {
        // many of these, allocate only on success
        child = builder->makeNop();
      }
    }
  }
  void visitIf(If* curr) {
    handleCondition(curr->condition);
  }
  void visitBreak(Break* curr) {
    handleCondition(curr->condition);
  }
  void visitSwitch(Switch* curr) {
    // TODO: try more values in condition? but may be very large set of values
    handleCondition(curr->condition);
  }
  void visitCall(Call* curr) {
    handleConcreteCurrentValue();
  }
  void visitCallImport(CallImport* curr) {
    handleConcreteCurrentValue();
  }
  void visitCallIndirect(CallIndirect* curr) {
    handleConcreteCurrentValue();
  }
  void visitGetLocal(GetLocal* curr) {
    handleConcreteCurrentValue();
  }
  void visitSetLocal(SetLocal* curr) {
    if (curr->isTee()) {
      // maybe we don't need the set
      if (tryToReplaceCurrent(curr->value)) return;
    }
    handleConcreteCurrentValue();
  }
  void visitGetGlobal(GetGlobal* curr) {
    handleConcreteCurrentValue();
  }
  void visitLoad(Load* curr) {
    handleConcreteCurrentValue();
  }
  void visitConst(Const* curr) {
    handleConcreteCurrentValue();
  }
  void visitUnary(Unary* curr) {
    handleConcreteCurrentValue();
  }
  void visitBinary(Binary* curr) {
    handleConcreteCurrentValue();
  }
  void visitSelect(Select* curr) {
    handleCondition(curr->condition);
  }

  void visitFunction(Function* curr) {
    funcsSeen++;
    static int last = 0;
    int percentage = (100 * funcsSeen) / getModule()->functions.size();
    if (percentage != last) {
      std::cout << "|    " << percentage << "% of funcs complete\n";
      last = percentage;
    }
  }

  // helpers

  // try to replace condition with always true and always false
  void handleCondition(Expression*& condition) {
    if (!condition) return;
    if (condition->is<Const>()) return;
    auto* c = builder->makeConst(Literal(int32_t(0)));
    if (!tryToReplaceChild(condition, c)) {
      c->value = Literal(int32_t(1));
      tryToReplaceChild(condition, c);
    }
  }

  // try to replace a concrete value with a trivial constant
  void handleConcreteCurrentValue() {
    auto* curr = getCurrent();
    if (!isConcreteWasmType(curr->type)) return;
    if (curr->is<Const>()) return;
    // try to replace with a trivial value
    Const* c = builder->makeConst(Literal(int32_t(0)));
    if (tryToReplaceCurrent(c)) return;
    c->value = LiteralUtils::makeLiteralFromInt32(1, curr->type);
    c->type = curr->type;
    tryToReplaceCurrent(c);
  }
};

//
// main
//

int main(int argc, const char* argv[]) {
  std::string input, test, working, command;
  Options options("wasm-reduce", "Reduce a wasm file to a smaller one that has the same behavior on a given command");
  options
      .add("--command", "-cmd", "The command to run on the test, that we want to reduce while keeping the command's output identical. "
                                "We look at the command's return code and stdout here (TODO: stderr), "
                                "and we reduce while keeping those unchanged.",
           Options::Arguments::One,
           [&](Options* o, const std::string& argument) {
             command = argument;
           })
      .add("--test", "-t", "Test file (this will be written to to test, the given command should read it when we call it)",
           Options::Arguments::One,
           [&](Options* o, const std::string& argument) {
             test = argument;
           })
      .add("--working", "-w", "Working file (this will contain the current good state while doing temporary computations, "
                              "and will contain the final best result at the end)",
           Options::Arguments::One,
           [&](Options* o, const std::string& argument) {
             working = argument;
           })
      .add_positional("INFILE", Options::Arguments::One,
                      [&](Options* o, const std::string& argument) {
                        input = argument;
                      });
  options.parse(argc, argv);

  if (test.size() == 0) Fatal() << "test file not provided\n";
  if (working.size() == 0) Fatal() << "working file not provided\n";

  std::cout << "|input: " << input << '\n';
  std::cout << "|test: " << test << '\n';
  std::cout << "|working: " << working << '\n';

  // get the expected output
  copy_file(input, test);
  expected.getFromExecution(command);

  std::cout << "|expected result:\n";
  expected.dump();

  // sanity check - we should start with an invalid module, and one
  // that we can read and write TODO: allow reducing things we can't
  // even do that with
  std::cout << "|checking that command has expected behavior on canonicalized (read-written) binary\n";
  {
    canonicalize(input, test);
    ProgramResult result(command);
    if (result != expected) {
      result.dump();
      Fatal() << "running command on the canonicalized module should give the same results";
    }
  }

  // copy test file (that was just written) to working, which we will use from now on
  // starting from the binary binaryen output canonicalizes, so we are not on text
  // input or input from somewhere else
  copy_file(test, working);
  std::cout << "|canonicalized input size: " << file_size(working) << "\n";

  std::cout << "|starting reduction!\n";

  unsigned currSize = 0;

  while (1) {
    Reducer reducer(command, test, working);

    // run binaryen optimization passes to reduce. passes are fast to run
    // and can often reduce large amounts of code efficiently, as opposed
    // to detructive reduction (i.e., that doesn't preserve correctness as
    // passes do) since destrucive must operate one change at a time
    std::cout << "|  reduce using passes...\n";
    reducer.reduceUsingPasses();

    // after we did a destructive reduction, and ran passes, the binary
    // size should shrink. if that combo increased binary size, then we
    // are at risk of an infinite loop
    if (currSize != 0 && file_size(working) >= currSize) {
      //break;
    }

    std::cout << "|  reduce destructively...\n";
    if (!reducer.reduceDestructively()) break;

    currSize = file_size(working);
    std::cout << "|  destructive reduction led to size: " << currSize << '\n';
  }
  std::cout << "|finished, final size: " << file_size(working) << "\n";
  copy_file(working, test); // just to avoid confusion
}

