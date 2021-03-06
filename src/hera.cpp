/*
 * Copyright 2016-2018 Alex Beregszaszi et al.
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

#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <string.h>
#include <fstream>
#include <sstream>


#ifdef HAVE_UNISTD
#include <unistd.h>
#endif

#include <io.h>

#include <binaryen/pass.h>
#include <binaryen/wasm.h>
#include <binaryen/wasm-binary.h>
#include <binaryen/wasm-builder.h>
#include <binaryen/wasm-interpreter.h>
#include <binaryen/wasm-printing.h>
#include <binaryen/wasm-validator.h>

#include <evmc/evmc.h>
#include <evmc/evm2wasm.h>

#include "hera.h"
#include "eei.h"
#include "exceptions.h"

using namespace std;
using namespace wasm;
using namespace HeraVM;

enum class hera_wasm_engine {
  binaryen,
  wavm,
  wabt
};

enum hera_evm_mode {
  EVM_REJECT,
  EVM_FALLBACK,
  EVM2WASM_CONTRACT,
  EVM2WASM_CPP,
  EVM2WASM_CPP_TRACING,
  EVM2WASM_JS,
  EVM2WASM_JS_TRACING
};

struct hera_instance : evmc_instance {
  hera_wasm_engine wasm_engine = hera_wasm_engine::binaryen;
  hera_evm_mode evm_mode = EVM_REJECT;
  bool metering = false;

  hera_instance() : evmc_instance({EVMC_ABI_VERSION, "hera", "0.0.0", nullptr, nullptr, nullptr, nullptr}) {}
};

namespace {

bool hasWasmPreamble(vector<uint8_t> const& _input) {
  return
    _input.size() >= 8 &&
    _input[0] == 0 &&
    _input[1] == 'a' &&
    _input[2] == 's' &&
    _input[3] == 'm' &&
    _input[4] == 1 &&
    _input[5] == 0 &&
    _input[6] == 0 &&
    _input[7] == 0;
}

// Calls a system contract at @address with input data @input.
// It is a "staticcall" with sender 000...000 and no value.
// @returns output data from the contract and update the @gas variable with the gas left.
vector<uint8_t> callSystemContract(
  evmc_context* context,
  evmc_address const& address,
  int64_t & gas,
  vector<uint8_t> const& input
) {
  evmc_message message = {
    /*.destination = */ address,
    /* .sender = */ {},
    /* .value = */ {},
    /* .input_data = */ input.data(),
    /* .input_size = */ input.size(),
    /* .code_hash = */ {},
    /* .create2_salt = */ {},
    /* .gas = */ gas,
    /*.depth = */ 0,
    /* .kind = */ EVMC_CALL,
    /* .flags = */ EVMC_STATIC
  };

  evmc_result result;
  context->fn_table->call(&result, context, &message);

  vector<uint8_t> ret;
  if (result.status_code == EVMC_SUCCESS && result.output_data)
    ret.assign(result.output_data, result.output_data + result.output_size);

  gas = result.gas_left;

  if (result.release)
    result.release(&result);

  return ret;
}

// Calls the Sentinel contract with input data @input.
// @returns the validated and metered output or empty output otherwise.
vector<uint8_t> sentinel(evmc_context* context, vector<uint8_t> const& input)
{
#if HERA_DEBUGGING
  cerr << "Metering (input " << input.size() << " bytes)..." << endl;
#endif

  int64_t startgas = numeric_limits<int64_t>::max(); // do not charge for metering yet (give unlimited gas)
  int64_t gas = startgas;
  vector<uint8_t> ret = callSystemContract(
    context, { 
       /*.bytes =*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xa } 
    }, // precompile address 0x00...0a
    gas,
    input
  );

#if HERA_DEBUGGING
  cerr << "Metering done (output " << ret.size() << " bytes, used " << (startgas - gas) << " gas)" << endl;
#endif

  return ret;
}

// NOTE: assumes that pattern doesn't contain any formatting characters (e.g. %)
string mktemp_string(string pattern) {
  char* tmp = new char(pattern.size() + 1);
  strcpy(tmp, pattern.data());
  string s = ((mktemp(tmp) && strlen(tmp))) ? string(tmp, strlen(tmp)) : string();
  delete tmp;
  return s;
}

// Calls evm2wasm (as a Javascript CLI) with input data @input.
// @returns the compiled output or empty output otherwise.
vector<uint8_t> evm2wasm_js(vector<uint8_t> const& input, bool evmTrace) {
#if HERA_DEBUGGING
  cerr << "Calling evm2wasm.js (input " << input.size() << " bytes)..." << endl;
#endif

  string fileEVM = mktemp_string("/tmp/hera.evm2wasm.evm.XXXXXX");
  string fileWASM = mktemp_string("/tmp/hera.evm2wasm.wasm.XXXXXX");

  if (fileEVM.size() == 0 || fileWASM.size() == 0)
    return vector<uint8_t>();

  ofstream os;
  os.open(fileEVM);
  // print as a hex sting
  os << hex;
  for (uint8_t byte: input)
    os << setfill('0') << setw(2) << static_cast<int>(byte);
  os.close();

  string cmd = string("evm2wasm.js ") + "-e " + fileEVM + " -o " + fileWASM + " --charge-per-op";
  if (evmTrace)
    cmd += " --trace";

#if HERA_DEBUGGING
  cerr << "(Calling evm2wasm.js with command: " << cmd << ")" << endl;
#endif

  int ret = system(cmd.data());
  unlink(fileEVM.data());

  if (ret != 0) {
#if HERA_DEBUGGING
    cerr << "evm2wasm.js failed" << endl;
#endif

    unlink(fileWASM.data());
    return vector<uint8_t>();
  }

  ifstream is(fileWASM);
  string str((istreambuf_iterator<char>(is)),
                 istreambuf_iterator<char>());

  unlink(fileWASM.data());

#if HERA_DEBUGGING
  cerr << "evm2wasm.js done (output " << str.length() << " bytes)" << endl;
#endif

  return vector<uint8_t>(str.begin(), str.end());
}

// Calls evm2wasm (through the built-in C++ interface) with input data @input.
// @returns the compiled output or empty output otherwise.
vector<uint8_t> evm2wasm_cpp(vector<uint8_t> const& input, bool evmTrace) {
#if HERA_DEBUGGING
  cerr << "Calling evm2wasm.cpp (input " << input.size() << " bytes)..." << endl;
#endif

  string str = evm2wasm::evm2wasm(input, evmTrace);

#if HERA_DEBUGGING
  cerr << "evm2wasm.cpp done (output " << str.length() << " bytes)" << endl;
#endif

  return vector<uint8_t>(str.begin(), str.end());
}

// Calls the evm2wasm contract with input data @input.
// @returns the compiled output or empty output otherwise.
vector<uint8_t> evm2wasm(evmc_context* context, vector<uint8_t> const& input) {
#if HERA_DEBUGGING
  cerr << "Calling evm2wasm (input " << input.size() << " bytes)..." << endl;
#endif

  int64_t startgas = numeric_limits<int64_t>::max(); // do not charge for metering yet (give unlimited gas)
  int64_t gas = startgas;
  vector<uint8_t> ret = callSystemContract(
    context,
    { /*.bytes =*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xb } }, // precompile address 0x00...0b
    gas,
    input
  );

#if HERA_DEBUGGING
  cerr << "evm2wasm done (output " << ret.size() << " bytes, used " << (startgas - gas) << " gas)" << endl;
#endif

  return ret;
}

// NOTE: This should be caught during deployment time by the Sentinel.
void validate_contract(Module & module)
{
  ensureCondition(
    module.getExportOrNull(Name("main")) != nullptr,
    ContractValidationFailure,
    "Contract entry point (\"main\") missing."
  );

  ensureCondition(
    module.getExportOrNull(Name("memory")) != nullptr,
    ContractValidationFailure,
    "Contract export (\"memory\") missing."
  );

  ensureCondition(
    module.exports.size() == 2,
    ContractValidationFailure,
    "Contract exports more than (\"main\") and (\"memory\")."
  );

  for (auto const& import: module.imports) {
     ensureCondition(
        import->module == Name("ethereum"),
        ContractValidationFailure,
        "Import from invalid namespace."
     );

     /*
    ensureCondition(
      import->module == Name("ethereum")
#if HERA_DEBUGGING
      || import->module == Name("debug")
#endif
      ,
      ContractValidationFailure,
      "Import from invalid namespace."
    );*/

  }
}

// Execute the contract through Binaryen.
void execute(
  evmc_context* context,
  vector<uint8_t> const& code,
  vector<uint8_t> const& state_code,
  evmc_message const& msg,
  ExecutionResult & result,
  bool meterInterfaceGas
) {
#if HERA_DEBUGGING
  cerr << "Executing..." << endl;
#endif

  Module module;

  // Load module
  try {
    WasmBinaryBuilder parser(module, reinterpret_cast<vector<char> const&>(code), false);
    parser.read();
  } catch (ParseException const& e) {
    ensureCondition(
      false,
      ContractValidationFailure,
      "Error in parsing WASM binary: '" +
      e.text +
      "' at " +
      to_string(e.line) +
      ":" +
      to_string(e.col)
    );
  }

  // Print
  // WasmPrinter::printModule(module);

  // Validate
  ensureCondition(
    WasmValidator().validate(module),
    ContractValidationFailure,
    "Module is not valid."
  );

  // NOTE: This should be caught during deployment time by the Sentinel.
  validate_contract(module);

  // NOTE: DO NOT use the optimiser here, it will conflict with metering

  // Interpret
  EthereumInterface interface(context, state_code, msg, result, meterInterfaceGas);
  ModuleInstance instance(module, &interface);

  Name main = Name("main");
  LiteralList args;
  instance.callExport(main, args);
}

void hera_destroy_result(evmc_result const* result) noexcept
{
  delete[] result->output_data;
}

evmc_result hera_execute(
  evmc_instance *instance,
  evmc_context *context,
  enum evmc_revision rev,
  const evmc_message *msg,
  const uint8_t *code,
  size_t code_size
) noexcept {
  hera_instance* hera = static_cast<hera_instance*>(instance);

  evmc_result ret;
  memset(&ret, 0, sizeof(evmc_result));

  try {
    heraAssert(rev == EVMC_BYZANTIUM, "Only Byzantium supported.");
    heraAssert(msg->gas >= 0, "Negative startgas?");

    bool meterInterfaceGas = true;

    // the bytecode residing in the state - this will be used by interface methods (i.e. codecopy)
    vector<uint8_t> state_code(code, code + code_size);

    // the actual executable code - this can be modified (metered or evm2wasm compiled)
    vector<uint8_t> _code(code, code + code_size);

    // ensure we can only handle WebAssembly version 1
    if (!hasWasmPreamble(_code)) {
      switch (hera->evm_mode) {
      case EVM2WASM_CONTRACT:
        _code = evm2wasm(context, _code);
        ensureCondition(_code.size() > 5, ContractValidationFailure, "Transcompiling via evm2wasm failed");
        // TODO: enable this once evm2wasm does metering of interfaces
        // meterInterfaceGas = false;
        break;
      case EVM2WASM_CPP:
      case EVM2WASM_CPP_TRACING:
        _code = evm2wasm_cpp(_code, hera->evm_mode == EVM2WASM_CPP_TRACING);
        ensureCondition(_code.size() > 5, ContractValidationFailure, "Transcompiling via evm2wasm.cpp failed");
        // TODO: enable this once evm2wasm does metering of interfaces
        // meterInterfaceGas = false;
        break;
      case EVM2WASM_JS:
      case EVM2WASM_JS_TRACING:
        _code = evm2wasm_js(_code, hera->evm_mode == EVM2WASM_JS_TRACING);
        ensureCondition(_code.size() > 5, ContractValidationFailure, "Transcompiling via evm2wasm.js failed");
        // TODO: enable this once evm2wasm does metering of interfaces
        // meterInterfaceGas = false;
        break;
      case EVM_FALLBACK:
        ret.status_code = EVMC_REJECTED;
        return ret;
      case EVM_REJECT:
        ret.status_code = EVMC_FAILURE;
        return ret;
      default:
        heraAssert(false, "");
      }
    } else if (msg->kind == EVMC_CREATE) {
      // Meter the deployment (constructor) code if it is WebAssembly
      if (hera->metering)
        _code = sentinel(context, _code);
      ensureCondition(_code.size() > 5, ContractValidationFailure, "Invalid contract or metering failed.");
    }

    heraAssert(hera->wasm_engine == hera_wasm_engine::binaryen, "Unsupported wasm engine.");

    ExecutionResult result;
    execute(context, _code, state_code, *msg, result, meterInterfaceGas);

    // copy call result
    if (result.returnValue.size() > 0) {
      vector<uint8_t> returnValue;

      if (msg->kind == EVMC_CREATE && !result.isRevert && hasWasmPreamble(result.returnValue)) {
        // Meter the deployed code if it is WebAssembly
        returnValue = hera->metering ? sentinel(context, result.returnValue) : move(result.returnValue);
        ensureCondition(returnValue.size() > 5, ContractValidationFailure, "Invalid contract or metering failed.");
      } else {
        returnValue = move(result.returnValue);
      }

      uint8_t* output_data = new uint8_t[returnValue.size()];
      copy(returnValue.begin(), returnValue.end(), output_data);

      ret.output_size = returnValue.size();
      ret.output_data = output_data;
      ret.release = hera_destroy_result;
    }

    ret.status_code = result.isRevert ? EVMC_REVERT : EVMC_SUCCESS;
    ret.gas_left = result.gasLeft;
  } catch (OutOfGas const& e) {
    ret.status_code = EVMC_OUT_OF_GAS;
#if HERA_DEBUGGING
    cerr << e.what() << endl;
#endif
  } catch (ContractValidationFailure const& e) {
    ret.status_code = EVMC_CONTRACT_VALIDATION_FAILURE;
#if HERA_DEBUGGING
    cerr << e.what() << endl;
#endif
  } catch (InvalidMemoryAccess const& e) {
    ret.status_code = EVMC_INVALID_MEMORY_ACCESS;
#if HERA_DEBUGGING
    cerr << e.what() << endl;
#endif
  } catch (StaticModeViolation const& e) {
    ret.status_code = EVMC_STATIC_MODE_VIOLATION;
#if HERA_DEBUGGING
    cerr << e.what() << endl;
#endif
  } catch (InternalErrorException const& e) {
    ret.status_code = EVMC_INTERNAL_ERROR;
#if HERA_DEBUGGING
    cerr << "InternalError: " << e.what() << endl;
#endif
  } catch (exception const& e) {
    ret.status_code = EVMC_INTERNAL_ERROR;
#if HERA_DEBUGGING
    cerr << "Unknown exception: " << e.what() << endl;
#endif
  } catch (...) {
    ret.status_code = EVMC_INTERNAL_ERROR;
#if HERA_DEBUGGING
    cerr << "Totally unknown exception" << endl;
#endif
  }

  return ret;
}

int hera_set_option(
  evmc_instance *instance,
  char const *name,
  char const *value
) noexcept {
  hera_instance* hera = static_cast<hera_instance*>(instance);
  if (strcmp(name, "fallback") == 0) {
    if (strcmp(value, "true") == 0)
      hera->evm_mode = EVM_FALLBACK;
    return 1;
  }

  if (strcmp(name, "evm2wasm") == 0) {
    if (strcmp(value, "true") == 0)
      hera->evm_mode = EVM2WASM_CONTRACT;
    return 1;
  }

  if (strcmp(name, "evm2wasm.cpp") == 0) {
    if (strcmp(value, "true") == 0)
      hera->evm_mode = EVM2WASM_CPP;
    return 1;
  }

  if (strcmp(name, "evm2wasm.cpp-trace") == 0) {
    if (strcmp(value, "true") == 0)
      hera->evm_mode = EVM2WASM_CPP_TRACING;
    return 1;
  }

  if (strcmp(name, "evm2wasm.js") == 0) {
    if (strcmp(value, "true") == 0)
      hera->evm_mode = EVM2WASM_JS;
    return 1;
  }

  if (strcmp(name, "evm2wasm.js-trace") == 0) {
    if (strcmp(value, "true") == 0)
      hera->evm_mode = EVM2WASM_JS_TRACING;
    return 1;
  }

  if (strcmp(name, "metering") == 0) {
    hera->metering = strcmp(value, "true") == 0;
    return 1;
  }

  if (strcmp(name, "engine") == 0) {
     if (strcmp(value, "binaryen") == 0)
       hera->wasm_engine = hera_wasm_engine::binaryen;
#if HAVE_WABT
     if (strcmp(value, "wabt") == 0)
       hera->wasm_engine = hera_wasm_engine::wabt;
#endif
#if HAVE_WAVM
     if (strcmp(value, "wavm") == 0)
       hera->wasm_engine = hera_wasm_engine::wavm;
#endif
     return 1;
  }

  return 0;
}

void hera_destroy(evmc_instance* instance) noexcept
{
  hera_instance* hera = static_cast<hera_instance*>(instance);
  delete hera;
}

} // anonymous namespace

extern "C" {

evmc_instance* evmc_create_hera() noexcept
{
  hera_instance* instance = new hera_instance;
  instance->destroy = hera_destroy;
  instance->execute = hera_execute;
  instance->set_option = hera_set_option;
  return instance;
}

}
