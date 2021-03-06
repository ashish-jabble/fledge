/*
 * Fledge filter plugin interface related
 *
 * Copyright (c) 2019 Dianomic Systems
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */

#include <logger.h>
#include <config_category.h>
#include <reading.h>
#include <reading_set.h>
#include <mutex>
#include <plugin_handle.h>
#include <Python.h>

#include <python_plugin_common_interface.h>
#include <reading_set.h>
#include <filter_plugin.h>

using namespace std;

extern "C" {

// This is a C++ ReadingSet class instance passed through
typedef ReadingSet READINGSET;
// Data handle passed to function pointer
typedef void OUTPUT_HANDLE;
// Function pointer called by "plugin_ingest" plugin method
typedef void (*OUTPUT_STREAM)(OUTPUT_HANDLE *, READINGSET *);

extern PLUGIN_INFORMATION *Py2C_PluginInfo(PyObject *);
extern void logErrorMessage();
extern PLUGIN_INFORMATION *plugin_info_fn();
extern void plugin_shutdown_fn(PLUGIN_HANDLE);
extern PyObject* createReadingsList(const vector<Reading *>& readings);
extern void setImportParameters(string& shimLayerPath, string& fledgePythonDir);

/**
 * Function to invoke 'plugin_reconfigure' function in python plugin
 *
 * @param    handle     The plugin handle from plugin_init_fn
 * @param    config     The new configuration, as string
 */
static void filter_plugin_reconfigure_fn(PLUGIN_HANDLE handle,
					 const std::string& config)
{
	if (!handle)
	{
		Logger::getLogger()->fatal("plugin_handle: filter_plugin_reconfigure_fn(): "
					   "handle is NULL");
		return;
	}

	if (!pythonHandles)
	{
		// Plugin name can not be logged here
		Logger::getLogger()->error("pythonHandles map is NULL "
					   "in filter_plugin_reconfigure_fn");
		return;
	}

	// Look for Python module, handle is the key
	auto it = pythonHandles->find(handle);
	if (it == pythonHandles->end() ||
	    !it->second)
	{
		// Plugin name can not be logged here
		Logger::getLogger()->fatal("filter_plugin_reconfigure_fn(): "
					   "pModule is NULL, handle %p",
					   handle);
		return;
	}

        // We have plugin name
        string pName = it->second->m_name;

	std::mutex mtx;
	PyObject* pFunc;
	lock_guard<mutex> guard(mtx);
	PyGILState_STATE state = PyGILState_Ensure();

	Logger::getLogger()->debug("plugin_handle: plugin_reconfigure(): "
				   "pModule=%p, *handle=%p, plugin '%s'",
				   it->second->m_module,
				   handle,
				   pName.c_str());

	// Fetch required method in loaded object
	pFunc = PyObject_GetAttrString(it->second->m_module, "plugin_reconfigure");
	if (!pFunc)
	{
		Logger::getLogger()->fatal("Cannot find method 'plugin_reconfigure' "
					   "in loaded python module '%s'",
					   pName.c_str());
	}

	if (!pFunc || !PyCallable_Check(pFunc))
	{
		// Failure
		if (PyErr_Occurred())
		{
			logErrorMessage();
		}

		Logger::getLogger()->fatal("Cannot call method plugin_reconfigure "
					   "in loaded python module '%s'",
					   pName.c_str());
		Py_CLEAR(pFunc);

		PyGILState_Release(state);
		return;
	}

	Logger::getLogger()->debug("plugin_reconfigure with %s", config.c_str());

	// Call Python method passing an object and a C string
	PyObject* pReturn = PyObject_CallFunction(pFunc,
						  "Os",
						  handle,
						  config.c_str());

	Py_CLEAR(pFunc);

	// Handle returned data
	if (!pReturn)
	{
		Logger::getLogger()->error("Called python script method plugin_reconfigure "
					   ": error while getting result object, plugin '%s'",
					   pName.c_str());
		logErrorMessage();
	}
	else
	{
		PyObject* tmp = (PyObject *)handle;
		// Check current handle is Dict and pReturn is a Dict too
		if (PyDict_Check(tmp) && PyDict_Check(pReturn))
		{
			// Clear Dict content
			PyDict_Clear(tmp);
			// Populate hadnle Dict with new data in pReturn
			PyDict_Update(tmp, pReturn);
			// Remove pReturn ojbect
			Py_CLEAR(pReturn);

			Logger::getLogger()->debug("plugin_handle: plugin_reconfigure(): "
						   "got updated handle from python plugin=%p, plugin '%s'",
						   handle,
						   pName.c_str());
		}
		else
		{
			 Logger::getLogger()->error("plugin_handle: plugin_reconfigure(): "
						    "got object type '%s' instead of Python Dict, "
						    "python plugin=%p, plugin '%s'",
						    Py_TYPE(pReturn)->tp_name,
					   	    handle,
					  	    pName.c_str());
		}
	}

	PyGILState_Release(state);
}

/**
 * Ingest data into filters chain
 *
 * @param    handle     The plugin handle returned from plugin_init
 * @param    data       The ReadingSet data to filter
 */
void filter_plugin_ingest_fn(PLUGIN_HANDLE handle, READINGSET *data)
{
        if (!handle)
	{
		Logger::getLogger()->fatal("plugin_handle: filter_plugin_ingest_fn(): "
					   "handle is NULL");
		return;
	}

	if (!pythonHandles)
	{
		// Plugin name can not be logged here
		Logger::getLogger()->error("pythonHandles map is NULL "
					   "in filter_plugin_ingest_fn");
		return;
	}

	auto it = pythonHandles->find(handle);
	if (it == pythonHandles->end() ||
	    !it->second)
	{
		// Plugin name can not be logged here
		Logger::getLogger()->fatal("plugin_handle: plugin_ingest(): "
					   "pModule is NULL");
		return;
	}

	// We have plugin name
	string pName = it->second->m_name;

	PyObject* pFunc;
	PyGILState_STATE state = PyGILState_Ensure();

	// Fetch required method in loaded object
	pFunc = PyObject_GetAttrString(it->second->m_module, "plugin_ingest");
	if (!pFunc)
	{
		Logger::getLogger()->fatal("Cannot find 'plugin_ingest' "
					   "method in loaded python module '%s'",
					   pName.c_str());
	}
	if (!pFunc || !PyCallable_Check(pFunc))
	{
		// Failure
		if (PyErr_Occurred())
		{
			logErrorMessage();
		}

		Logger::getLogger()->fatal("Cannot call method plugin_ingest"
					   "in loaded python module '%s'",
					   pName.c_str());
		Py_CLEAR(pFunc);

		PyGILState_Release(state);
		return;
	}

	// Call asset tracker
	vector<Reading *>* readings = ((ReadingSet *)data)->getAllReadingsPtr();
	for (vector<Reading *>::const_iterator elem = readings->begin();
						      elem != readings->end();
						      ++elem)
	{
		AssetTracker* atr = AssetTracker::getAssetTracker();
		if (atr)
		{
			AssetTracker::getAssetTracker()->addAssetTrackingTuple(it->second->getCategoryName(),
										(*elem)->getAssetName(),
										string("Filter"));
		}
	}

	// Create a dict of readings
	// - 1 - Create Python list of dicts as input to the filter
	PyObject* readingsList =
		createReadingsList(((ReadingSet *)data)->getAllReadings());

	PyObject* pReturn = PyObject_CallFunction(pFunc,
						  "OO",
						  handle,
						  readingsList);
	Py_CLEAR(pFunc);
	// Remove input data
	delete (ReadingSet *)data;
	data = NULL;

	// Handle returned data
	if (!pReturn)
	{
		Logger::getLogger()->error("Called python script method plugin_ingest "
					   ": error while getting result object, plugin '%s'",
					   pName.c_str());
		logErrorMessage();
	}

	// Remove readings to dict
	Py_CLEAR(readingsList);
	// Remove CallFunction result
	Py_CLEAR(pReturn);

	// Release GIL
	PyGILState_Release(state);
}

/**
 * Initialise the plugin, called to get the plugin handle and setup the
 * output handle that will be passed to the output stream. The output stream
 * is merely a function pointer that is called with the output handle and
 * the new set of readings generated by the plugin.
 *     (*output)(outHandle, readings);
 * Note that the plugin may not call the output stream if the result of
 * the filtering is that no readings are to be sent onwards in the chain.
 * This allows the plugin to discard data or to buffer it for aggregation
 * with data that follows in subsequent calls
 *
 * @param config	The configuration category for the filter
 * @param outHandle	A handle that will be passed to the output stream
 * @param output	The output stream (function pointer) to which data is passed
 * @return		An opaque handle that is used in all subsequent calls to the plugin
 */
PLUGIN_HANDLE filter_plugin_init_fn(ConfigCategory* config,
			  OUTPUT_HANDLE *outHandle,
			  OUTPUT_STREAM output)
{
	// Get pluginName
	string pName = config->getValue("plugin");

	if (!pythonModules)
	{
		Logger::getLogger()->error("pythonModules map is NULL "
					   "in filter_plugin_init_fn, plugin '%s'",
					   pName.c_str());
		return NULL;
	}

	bool loadModule = false;
	bool reloadModule = false;
	bool pythonInitState = false;
	PythonModule *module = NULL;
	PyThreadState* newInterp = NULL;

	// Check wether plugin pName has been already loaded
	for (auto h = pythonHandles->begin();
                  h != pythonHandles->end(); ++h)
        {
		if (h->second->m_name.compare(pName) == 0)
		{
			Logger::getLogger()->debug("filter_plugin_init_fn: already loaded "
						   "a plugin with name '%s'. A new Python obj is needed",
						   pName.c_str());

			// Set Python library loaded state
			pythonInitState = h->second->m_init;

			// Set load indicator
			loadModule = true;

			break;
		}
	}

	if (!loadModule)
	{
		// Plugin name not previously loaded: check current Python module
		// pName is the key
		auto it = pythonModules->find(pName);
		if (it == pythonModules->end())
		{
			Logger::getLogger()->debug("plugin_handle: filter_plugin_init(): "
						   "pModule not found for plugin '%s': "
						   "import Python module using a new interpreter.",
						   pName.c_str());

			// Set reload indicator
			reloadModule = true;
		}
		else
		{
			if (it->second && it->second->m_module)
			{
				// Just use current loaded module: no load or re-load action
				module = it->second;

				// Set Python library loaded state
				pythonInitState = it->second->m_init;
			}
			else
			{
				Logger::getLogger()->fatal("plugin_handle: filter_plugin_init(): "
							   "found pModule is NULL for plugin '%s': ",
							   pName.c_str());
				return NULL;
			}
		}
	}

	// Acquire GIL
	PyGILState_STATE state = PyGILState_Ensure();

	// Import Python module using a new interpreter
	if (loadModule || reloadModule)
	{
		// Start a new interpreter
		newInterp = Py_NewInterpreter();
		if (!newInterp)
		{
			Logger::getLogger()->fatal("plugin_handle: filter_plugin_init() "
						   "Py_NewInterpreter failure "
						   "for for plugin '%s': ",
						   pName.c_str());
			logErrorMessage();

			PyGILState_Release(state);
			return NULL;
		}

		string shimLayerPath;
		string fledgePythonDir;
		// Python 3.x set parameters for import
		setImportParameters(shimLayerPath, fledgePythonDir);

		string name(string(PLUGIN_TYPE_FILTER) + string(SHIM_SCRIPT_POSTFIX));

		// Set Python path for embedded Python 3.x
		// Get current sys.path - borrowed reference
		PyObject* sysPath = PySys_GetObject((char *)"path");
		PyList_Append(sysPath, PyUnicode_FromString((char *) fledgePythonDir.c_str()));
		PyList_Append(sysPath, PyUnicode_FromString((char *) shimLayerPath.c_str()));

		// Set sys.argv for embedded Python 3.x
		int argc = 2;
		wchar_t* argv[2];
		argv[0] = Py_DecodeLocale("", NULL);
		argv[1] = Py_DecodeLocale(pName.c_str(), NULL);

		// Set script parameters
		PySys_SetArgv(argc, argv);

		Logger::getLogger()->debug("%s_plugin_init_fn, %sloading plugin '%s', "
					   "using a new interpreter",
					   PLUGIN_TYPE_FILTER,
					   string(reloadModule ? "re-" : "").c_str(),
					   pName.c_str());

		// Import Python script
		PyObject *newObj = PyImport_ImportModule(name.c_str());

		// Check for NULL
		if (newObj)
		{
			PythonModule* newModule;
			if ((newModule = new PythonModule(newObj,
							  pythonInitState,
							  pName,
							  PLUGIN_TYPE_FILTER,
							  newInterp)) == NULL)
			{
				// Release lock
				PyEval_ReleaseThread(newInterp);

				Logger::getLogger()->fatal("plugin_handle: filter_plugin_init(): "
							   "failed to create Python module "
							   "object, plugin '%s'",
							   pName.c_str());
				return NULL;
			}

			// Set category name
			newModule->setCategoryName(config->getName());

			// Set module
			module = newModule;
		}
		else
		{
			logErrorMessage();

			// Release lock
			PyEval_ReleaseThread(newInterp);

			Logger::getLogger()->fatal("plugin_handle: filter_plugin_init(): "
						   "failed to import plugin '%s'",
						   pName.c_str());
			return NULL;
		}
	}
	else
	{
		// Set category name
		module->setCategoryName(config->getName());
	}

	Logger::getLogger()->debug("filter_plugin_init_fn for '%s', pModule '%p', "
				   "Python interpreter '%p'",
				   module->m_name.c_str(),
				   module->m_module,
				   module->m_tState);

        // Call Python method passing an object
        PyObject* ingest_fn = PyCapsule_New((void *)output, NULL, NULL);
        PyObject* ingest_ref = PyCapsule_New((void *)outHandle, NULL, NULL);
        PyObject* pReturn = PyObject_CallMethod(module->m_module,
						"plugin_init",
						"sOO",
						config->itemsToJSON().c_str(),
						ingest_ref,
						ingest_fn);

        Py_CLEAR(ingest_ref);
        Py_CLEAR(ingest_fn);


        // Handle returned data
        if (!pReturn)
        {
                Logger::getLogger()->error("Called python script method plugin_init "
                                           ": error while getting result object, plugin '%s'",
                                           pName.c_str());
                logErrorMessage();
        }
        else
        {
                Logger::getLogger()->debug("plugin_handle: filter_plugin_init(): "
                                           "got result object '%p', plugin '%s'",
                                           pReturn,
                                           pName.c_str());
        }

	// Add the handle to handles map as key, PythonModule object as value
	std::pair<std::map<PLUGIN_HANDLE, PythonModule*>::iterator, bool> ret;
	if (pythonHandles)
	{
		// Add to handles map the PythonHandles object
		ret = pythonHandles->insert(pair<PLUGIN_HANDLE, PythonModule*>
			((PLUGIN_HANDLE)pReturn, module));

		if (ret.second)
		{
			Logger::getLogger()->debug("plugin_handle: filter_plugin_init_fn(): "
						   "handle %p of python plugin '%s' "
						   "added to pythonHandles map",
						   pReturn,
						   pName.c_str());
		}
		else
		{
			Logger::getLogger()->error("plugin_handle: filter_plugin_init_fn(): "
						   "failed to insert handle %p of "
						   "python plugin '%s' to pythonHandles map",
						   pReturn,
						   pName.c_str());

			Py_CLEAR(module->m_module);
			module->m_module = NULL;
			delete module;
			module = NULL;

			Py_CLEAR(pReturn);
			pReturn = NULL;
		}
	}

	// Release locks
	if (newInterp)
	{
		PyEval_ReleaseThread(newInterp);
	}
	else
	{
		PyGILState_Release(state);
	}

	return pReturn ? (PLUGIN_HANDLE) pReturn : NULL;
}

/**
 * Constructor for PythonPluginHandle
 *    - Load python interpreter
 *    - Set sys.path and sys.argv
 *    - Import shim layer script and pass plugin name in argv[1]
 *
 * @param    pluginName         The plugin name to load
 * @param    pluginPathName     The plugin pathname
 * @return			PyObject of loaded module
 */
/**
 * Constructor for PythonPluginHandle
 *    - Load python interpreter
 *    - Set sys.path and sys.argv
 *    - Import shim layer script and pass plugin name in argv[1]
 *
 * @param    pluginName         The plugin name to load
 * @param    pluginPathName     The plugin pathname
 * @return                      PyObject of loaded module
 */
void *PluginInterfaceInit(const char *pluginName, const char * pluginPathName)
{
	bool initPython = false;

	// Set plugin name, also for methods in common-plugin-interfaces/python
	gPluginName = pluginName;

	string shimLayerPath;
	string fledgePythonDir;
	// Python 3.x set parameters for import
	setImportParameters(shimLayerPath, fledgePythonDir);

	string name(string(PLUGIN_TYPE_FILTER) + string(SHIM_SCRIPT_POSTFIX));

	// Embedded Python 3.x initialisation
	if (!Py_IsInitialized())
	{
		// Embedded Python 3.x program name
		wchar_t *programName = Py_DecodeLocale(name.c_str(), NULL);
		Py_SetProgramName(programName);
		PyMem_RawFree(programName);

		Py_Initialize();
		PyEval_InitThreads();
		PyThreadState* save = PyEval_SaveThread(); // release Python GIT
		initPython = true;
		Logger::getLogger()->debug("FilterPlugin PluginInterfaceInit "
					   "has loaded Python library, plugin '%s'",
					   pluginName); 
	}

	PyThreadState* newInterp = NULL;

	// Acquire GIL
	PyGILState_STATE state = PyGILState_Ensure();

	// New Python interpreter
	if (!initPython)
	{
		newInterp = Py_NewInterpreter();
		if (!newInterp)
		{
			Logger::getLogger()->fatal("FilterPlugin PluginInterfaceInit "
						   "Py_NewInterprete failure "
						   "for for plugin '%s': ",
						   pluginName);
			logErrorMessage();

			PyGILState_Release(state);
			return NULL;
		}

		Logger::getLogger()->debug("FilterPlugin PluginInterfaceInit "
					   "has added a new Python interpreter "
					   "'%p', plugin '%s'",
					   newInterp,
					   pluginName); 
	}

	Logger::getLogger()->debug("FilterPlugin PluginInterfaceInit %s:%d: "
				   "shimLayerPath=%s, fledgePythonDir=%s, plugin '%s'",
				   __FUNCTION__,
				   __LINE__,
				   shimLayerPath.c_str(),
				   fledgePythonDir.c_str(),
				   pluginName);

	// Set Python path for embedded Python 3.x
	// Get current sys.path - borrowed reference
	PyObject* sysPath = PySys_GetObject((char *)"path");
	PyList_Append(sysPath, PyUnicode_FromString((char *) shimLayerPath.c_str()));
	PyList_Append(sysPath, PyUnicode_FromString((char *) fledgePythonDir.c_str()));

	// Set sys.argv for embedded Python 3.x
	int argc = 2;
	wchar_t* argv[2];
	argv[0] = Py_DecodeLocale("", NULL);
	argv[1] = Py_DecodeLocale(pluginName, NULL);
	PySys_SetArgv(argc, argv);

	// 2) Import Python script
	PyObject *pModule = PyImport_ImportModule(name.c_str());

	// Check whether the Python module has been imported
	if (!pModule)
	{
		// Failure
		if (PyErr_Occurred())
		{
			logErrorMessage();
		}
		Logger::getLogger()->fatal("FilterPlugin PluginInterfaceInit: "
					   "cannot import Python shim file "
					   "'%s' from '%s', plugin '%s'",
					   name.c_str(),
					   shimLayerPath.c_str(),
					   pluginName);
	}
	else
	{
		std::pair<std::map<string, PythonModule*>::iterator, bool> ret;
		PythonModule* newModule = NULL;
		if (pythonModules)
		{
			// Add module into pythonModules, pluginName is the key
			PythonModule* newModule;
			if ((newModule = new PythonModule(pModule,
							  initPython,
							  string(pluginName),
							  PLUGIN_TYPE_FILTER,
							  newInterp)) == NULL)
			{
				// Release lock
				PyEval_ReleaseThread(newInterp);

				Logger::getLogger()->fatal("plugin_handle: filter_plugin_init(): "
							   "failed to create Python module "
							   "object, plugin '%s'",
							   pluginName);

				return NULL;
			}

			ret = pythonModules->insert(pair<string, PythonModule*>
				(string(pluginName), newModule));
		}

		// Check result
		if (!pythonModules ||
		    ret.second == false)
		{
			Logger::getLogger()->fatal("%s:%d: python module "
						   "not added to the map "
						   "of loaded plugins, "
						   "pModule=%p, plugin '%s', aborting.",
						   __FUNCTION__,
						   __LINE__,
						   pModule,
						   pluginName);
			// Cleanup
			Py_CLEAR(pModule);
			pModule = NULL;

			delete newModule;
			newModule = NULL;
		}
		else
		{
			Logger::getLogger()->debug("%s:%d: python module "
						   "successfully loaded, "
						   "pModule=%p, plugin '%s'",
						   __FUNCTION__,
						   __LINE__,
						   pModule,
						   pluginName);
		}
	}

	// Release locks
	if (!initPython)
	{
		PyEval_ReleaseThread(newInterp);
	}
	else
	{
		PyGILState_Release(state);
	}

	// Return new Python module or NULL
	return pModule;
}

/**
 * Returns function pointer that can be invoked to call '_sym' function
 * in python plugin
 *
 * @param    _sym	Symbol name
 * @param    pName	Plugin name
 * @return		function pointer to be invoked
 */
void *PluginInterfaceResolveSymbol(const char *_sym, const string& pName)
{
	string sym(_sym);
	if (!sym.compare("plugin_info"))
		return (void *) plugin_info_fn;
	else if (!sym.compare("plugin_init"))
		return (void *) filter_plugin_init_fn;
	else if (!sym.compare("plugin_shutdown"))
		return (void *) plugin_shutdown_fn;
	else if (!sym.compare("plugin_reconfigure"))
		return (void *) filter_plugin_reconfigure_fn;
	else if (!sym.compare("plugin_ingest"))
		return (void *) filter_plugin_ingest_fn;
	else if (!sym.compare("plugin_start"))
	{
		Logger::getLogger()->debug("FilterPluginInterface currently "
					   "does not support 'plugin_start', plugin '%s'",
					   pName.c_str());
		return NULL;
	}
	else
	{
		Logger::getLogger()->fatal("FilterPluginInterfaceResolveSymbol can not find symbol '%s' "
					   "in the Filter Python plugin interface library, "
					   "loaded plugin '%s'",
					   _sym,
					   pName.c_str());
		return NULL;
	}
}
}; // End of extern C
