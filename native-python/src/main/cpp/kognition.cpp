
#include <stdint.h>
#include <mutex>
#include "jfloats.h"
#include "PythonEnvironment.h"
#include "ImageSource.h"
#include "GilGuard.h"
#include "KogSystem.h"

#include "kog_exports.h"

int initModule_kognition();

static bool inited = false;

using namespace pilecv4j::python;

//static void dumpDict(PyObject* module) {
//  PyObject *key, *value;
//  Py_ssize_t pos = 0;
//
//  fprintf(stderr,"Dumping dict for %s\n", PyModule_GetName(module));
//  PyObject* dict = PyModule_GetDict(module);
//  if (!dict) {
//    log(ERROR, "Failed to get dict from module %s", PyModule_GetName(module));
//    return;
//  }
//  while (PyDict_Next(dict, &pos, &key, &value)) {
//    PyObject_Print(key, stderr, Py_PRINT_RAW);
//    fprintf(stderr, " = ");
//    PyObject_Print(value, stderr, Py_PRINT_RAW);
//    fprintf(stderr, "\n=============\n");
//    fflush(stderr);
//  }
//}

// this is in module.cpp
extern PyObject* convert(KogSystem* pt);

extern "C" {
  KAI_EXPORT char* pilecv4j_python_statusMessage(uint32_t status) {
    if (status == 0)
      return nullptr;

    return strdup(getStatusMessage(status));
  }

  KAI_EXPORT void pilecv4j_python_freeStatusMessage(char* messageRef) {
    if (messageRef)
      free((void*)messageRef);
  }

  KAI_EXPORT int32_t pilecv4j_python_initPython() {
    static std::mutex initMutex;

    log(DEBUG, "initPython called from java.");
    std::lock_guard<std::mutex> lck(initMutex);

    if (inited) {
      log(WARN, "Attempted call to initialize python more than once.");
      return OK;
    }

    // needs to be called before creating PythonEnvironment
    if (initModule_kognition() < 0)
      return (int32_t) FAILED_INSTALL_KOGNITION_MODULE;

    // This will instantiate a PythonEnvironment
    PythonEnvironment::instance();

    inited = true;
    return (int32_t)OK;
  }

  KAI_EXPORT uint64_t pilecv4j_python_initKogSys(get_image_source cb) {
    return (uint64_t)new KogSystem(cb);
  }

  KAI_EXPORT int32_t pilecv4j_python_kogSys_numModelLabels(uint64_t ptRef) {
    KogSystem* ths = (KogSystem*)ptRef;
    return ths->getNumLabels();
  }

  KAI_EXPORT const char* pilecv4j_python_kogSys_modelLabel(uint64_t ptRef, int32_t index) {
    KogSystem* ths = (KogSystem*)ptRef;
    return ths->getModelLabel(index);
  }

  KAI_EXPORT int32_t pilecv4j_python_closePyTorch(uint64_t ptRef) {
    delete (KogSystem*)ptRef;
    return OK;
  }

  KAI_EXPORT int32_t pilecv4j_python_runPythonFunction(const char* moduleName, const char* functionName, uint64_t paramDictRef) {
    PyObject* paramDict = (PyObject*)paramDictRef;
    PythonEnvironment* p = PythonEnvironment::instance();
    return p->runModel(moduleName, functionName, paramDict);
  }

  KAI_EXPORT uint64_t pilecv4j_python_imageSourceSend(uint64_t imageSourceRef, uint64_t matRef, int32_t rgbi) {
    ImageSource* is = ((ImageSource*)imageSourceRef);
    if (matRef == 0L) {
      is->send(nullptr);
      return 0L;
    }
    cv::Mat* mat = (cv::Mat*)matRef;
    KogMatWithResults* km = new KogMatWithResults(mat, rgbi ? true : false, false);
    is->send(km);
    return (uint64_t)km;
  }

  KAI_EXPORT void pilecv4j_python_addModulePath(const char* modPath) {
    PythonEnvironment::instance()->addModulePath(modPath);
  }

  KAI_EXPORT uint64_t pilecv4j_python_makeImageSource(uint64_t pt) {
    KogSystem* ptorch = (KogSystem*)pt;
    return uint64_t(new ImageSource());
  }

  KAI_EXPORT uint64_t pilecv4j_python_imageSourcePeek(uint64_t imageSourceRef) {
    ImageSource* is = ((ImageSource*)imageSourceRef);
    return (uint64_t)(is->peek());
  }

  KAI_EXPORT void pilecv4j_python_imageSourceClose(uint64_t imageSourceRef) {
    ImageSource* is = ((ImageSource*)imageSourceRef);
    delete is;
  }

  KAI_EXPORT void pilecv4j_python_kogMatResults_close(uint64_t nativeObj) {
    log(TRACE, "Closing KogMatWithResults at %ld", (long)nativeObj);
    if (nativeObj) {
      ((KogMatWithResults*)nativeObj)->decrement();
    }
  }

  KAI_EXPORT int32_t pilecv4j_python_kogMatResults_hasResult(uint64_t nativeObj) {
    log(TRACE, "hasResult on %ld", (long)(nativeObj));

    if (nativeObj)
      return ((KogMatWithResults*)nativeObj)->resultsSet ? 1 : 0;
    else
      return 0;
  }

  KAI_EXPORT int32_t pilecv4j_python_kogMatResults_isAbandoned(uint64_t nativeObj) {
    if (nativeObj)
      return ((KogMatWithResults*)nativeObj)->abandoned ? 1 : 0;
    else
      return 0;
  }

  KAI_EXPORT uint64_t pilecv4j_python_kogMatResults_getResults(uint64_t nativeObj) {
    if (nativeObj) {
      cv::Mat* results = ((KogMatWithResults*)nativeObj)->results;
      if (results)
        return (uint64_t)(new cv::Mat(*results));
      else
        return 0L;
    } else
      return 0L;
  }

  KAI_EXPORT uint64_t pilecv4j_python_newParamDict() {
    CallPythonGuard gg;
    return (uint64_t)(PyObject*) PyDict_New();
  }

  KAI_EXPORT void pilecv4j_python_closeParamDict(uint64_t dictRef) {
    CallPythonGuard gg;
    Py_DECREF((PyObject*)dictRef);
  }

  KAI_EXPORT int32_t pilecv4j_python_putBooleanParamDict(uint64_t dictRef, const char* key, int32_t valRef) {
    StatusCode result = OK;
    CallPythonGuard gg;
    PyObject* dict = (PyObject*)dictRef;
    PyObject* val = valRef ? Py_True : Py_False;
    Py_INCREF(val);
    if (PyDict_SetItemString(dict, key, val) != 0) {
      log(ERROR, "Failed to insert parameter (%s : %s) into dictionary", key, (valRef ? "True" : "False"));
      result = FAILED_TO_INSERT_INTO_DICTIONARY;
    }
    Py_DECREF(val);
    return (int32_t)result;
  }

  KAI_EXPORT int32_t pilecv4j_python_putIntParamDict(uint64_t dictRef, const char* key, int64_t valRef) {
    StatusCode result = OK;
    CallPythonGuard gg;
    PyObject* dict = (PyObject*)dictRef;
    PyObject* val = PyLong_FromLongLong((long long)valRef);
    if (PyDict_SetItemString(dict, key, val) != 0) {
      log(ERROR, "Failed to insert parameter (%s : %s) into dictionary", key, (valRef ? "True" : "False"));
      result = FAILED_TO_INSERT_INTO_DICTIONARY;
    }
    Py_DECREF(val);
    return (int32_t)result;
  }

  KAI_EXPORT int32_t pilecv4j_python_putFloatParamDict(uint64_t dictRef, const char* key, float64_t valRef) {
    StatusCode result = OK;
    CallPythonGuard gg;
    PyObject* dict = (PyObject*)dictRef;
    PyObject* val = PyFloat_FromDouble((double)valRef);
    if (PyDict_SetItemString(dict, key, val) != 0) {
      log(ERROR, "Failed to insert parameter (%s : %s) into dictionary", key, (valRef ? "True" : "False"));
      result = FAILED_TO_INSERT_INTO_DICTIONARY;
    }
    Py_DECREF(val);
    return (int32_t)result;
  }


  KAI_EXPORT int32_t pilecv4j_python_putStringParamDict(uint64_t dictRef, const char* key, const char* valRaw) {
    StatusCode result = OK;
    CallPythonGuard gg;
    PyObject* dict = (PyObject*)dictRef;
    PyObject* val = PyUnicode_FromString(valRaw);
    if (PyDict_SetItemString(dict, key, val) != 0) {
      log(ERROR, "Failed to insert parameter (%s : %s) into dictionary", key, valRaw);
      result = FAILED_TO_INSERT_INTO_DICTIONARY;
    }
    Py_DECREF(val);
    return (int32_t)result;
  }

  KAI_EXPORT int32_t pilecv4j_python_putPytorchParamDict(uint64_t dictRef, const char* key, uint64_t valRef) {

    CallPythonGuard gg;
    PythonEnvironment::instance()->loadKognitionModule();
    PyObject* pytorch = convert((KogSystem*)valRef);
    if (!pytorch)
      return CANT_INSTANTIATE_PYTHON_OBJECT;

    int result = OK;
    if (!pytorch) {
      log (ERROR, "Failed to convert a PyTorch instance to a PyObject");
      return CANT_INSTANTIATE_PYTHON_OBJECT;
    }
    PyObject* dict = (PyObject*)dictRef;
    if (PyDict_SetItemString(dict, key, pytorch) != 0) {
      log(ERROR, "Failed to insert parameter (%s : %s) into dictionary", key, valRef);
      result = FAILED_TO_INSERT_INTO_DICTIONARY;
    }
    Py_DECREF(pytorch);
    return result;
  }

  KAI_EXPORT int32_t pilecv4j_python_setLogLevel(int32_t plogLevel) {
    if (plogLevel <= MAX_LOG_LEVEL && plogLevel >= 0)
      setLogLevel(static_cast<LogLevel>(plogLevel));
    else
      setLogLevel(FATAL);

    return OK;
  }
}

