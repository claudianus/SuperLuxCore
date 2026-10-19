/***************************************************************************
 * Copyright 1998-2025 by authors (see AUTHORS.txt)            *
 *                                     *
 *   This file is part of LuxCoreRender.                   *
 *                                     *
 * Licensed under the Apache License, Version 2.0 (the "License");     *
 * you may not use this file except in compliance with the License.    *
 * You may obtain a copy of the License at                 *
 *                                     *
 *   http://www.apache.org/licenses/LICENSE-2.0              *
 *                                     *
 * Unless required by applicable law or agreed to in writing, software   *
 * distributed under the License is distributed on an "AS IS" BASIS,     *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and   *
 * limitations under the License.                      *
 ***************************************************************************/
#include "luxrays/core/exttrianglemesh.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/utils/properties.h"
#include <iterator>
#include <pybind11/detail/common.h>
#include <pybind11/detail/using_smart_holder.h>
#include <string_view>

#ifdef WIN32
// Python 3.8 and older define snprintf as a macro even for VS 2015 and newer
// where this causes an error - See https://bugs.python.org/issue36020
#if defined(_MSC_VER) && _MSC_VER >= 1900
#define HAVE_SNPRINTF
#endif
#endif

#include <locale>
#include <memory>
#include <cmath>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <pybind11/operators.h>
#include <pybind11/typing.h>

#include <openvdb/openvdb.h>
#include "slg/textures/openvdbgrid.h"
#include <openvdb/io/File.h>


#include "luxrays/luxrays.h"
#include "luxcore/luxcore.h"
#include "luxcore/luxcoreimpl.h"
#include "luxcore/pysuperluxcore/pysuperluxcoreforblender.h"
#include "luxcore/pysuperluxcore/pysuperluxcoreutils.h"
#include "mikktspace.h"
#include "slg/bsdf/bsdf.h"
#include "slg/cameras/perspective.h"
#include "slg/film/imagepipeline/imagepipeline.h"
#include "slg/film/denoiser/denoisingframe.h"
#include "slg/film/denoiser/presentationresample.h"
#if !defined(LUXRAYS_DISABLE_OPENCL)
#include "slg/engines/pathoclbase/pathoclbase.h"
#include "slg/engines/rtpathocl/rtpathocl.h"
#endif

using namespace luxcore;
using namespace luxcore::detail;
namespace py = pybind11;

using PropertyUPtr = std::unique_ptr<luxrays::Property>;
using PropertyRPtr = const std::unique_ptr<luxrays::Property> &;
using SceneImplPtr = std::unique_ptr<luxcore::detail::SceneImpl>;
using PropertiesUPtr = std::unique_ptr<luxrays::Properties>;


namespace luxcore {

//------------------------------------------------------------------------------
// Module functions
//------------------------------------------------------------------------------

static std::mutex luxCoreInitMutex;
static py::object luxCoreLogHandler;

#if (PY_VERSION_HEX >= 0x03040000)
static void PythonDebugHandler(const char *msg) {
  // PyGILState_Check() is available since Python 3.4
  if (PyGILState_Check())
    luxCoreLogHandler(std::string(msg));
  else {
    // The following code is supposed to work ... but it doesn't (it never
    // returns when the thread that holds the GIL waits for this one). So
    // I'm just avoiding to call Python without the GIL. However, discarding
    // the message entirely hides every render thread line (kernel
    // compilation, errors, etc.) as well as anything logged while the GIL
    // is released during the long Start()/Stop() calls: keep those visible
    // on the process stderr instead.

    //PyGILState_STATE state = PyGILState_Ensure();
    //luxCoreLogHandler(std::string(msg));
    //PyGILState_Release(state);

    std::cerr << msg << std::endl;
  }
}
#else
static int PyGILState_Check2(void) {
  PyThreadState *tstate = _PyThreadState_Current;
  return tstate && (tstate == PyGILState_GetThisThreadState());
}

static void PythonDebugHandler(const char *msg) {
  if (PyGILState_Check2())
    luxCoreLogHandler(std::string(msg));
}
#endif

static void LuxCore_Init() {
  std::unique_lock<std::mutex> lock(luxCoreInitMutex);
  Init();
}

static void LuxCore_InitDefaultHandler(py::object &logHandler) {
  std::unique_lock<std::mutex> lock(luxCoreInitMutex);
  // I wonder if I should increase the reference count for Python
  luxCoreLogHandler = logHandler;

  Init(&PythonDebugHandler);
}

static void LuxCore_SetLogHandler(py::object &logHandler) {
  luxCoreLogHandler = logHandler;

  if (logHandler.is_none())
    SetLogHandler(NULL);
}

static const char *LuxCoreVersion() {
  static const char *luxCoreVersion = LUXCORE_VERSION;
  return luxCoreVersion;
}

static py::list GetOpenCLDeviceList() {
  luxrays::Context ctx;
  auto deviceDescriptions = ctx.GetAvailableDeviceDescriptions();

  // Select only OpenCL devices
  luxrays::DeviceDescription::Filter(
		(luxrays::DeviceType)(luxrays::DEVICE_TYPE_OPENCL_ALL | luxrays::DEVICE_TYPE_CUDA_ALL),
		deviceDescriptions
	);

  // Add all device information to the list
  py::list l;
  for (luxrays::DeviceDescriptionRef desc : deviceDescriptions) {
    l.append(py::make_tuple(
        desc.GetName(),
        luxrays::DeviceDescription::GetDeviceType(desc.GetType()),
        desc.GetComputeUnits(),
        desc.GetNativeVectorWidthFloat(),
        desc.GetMaxMemory(),
        desc.GetMaxMemoryAllocSize()));
  }

  return l;
}

static py::list GetDeviceList() {
  // Every compute device the runtime exposes, whatever the backend.
  // GetOpenCLDeviceList() filters down to OpenCL/CUDA, so on a Metal-only
  // machine (Apple silicon without an OpenCL ICD) it reports nothing while
  // the engine is perfectly able to render on the GPU.
  luxrays::Context ctx;
  auto deviceDescriptions = ctx.GetAvailableDeviceDescriptions();

  py::list l;
  for (luxrays::DeviceDescriptionRef desc : deviceDescriptions) {
    l.append(py::make_tuple(
        desc.GetName(),
        luxrays::DeviceDescription::GetDeviceType(desc.GetType()),
        desc.GetComputeUnits(),
        desc.GetNativeVectorWidthFloat(),
        desc.GetMaxMemory(),
        desc.GetMaxMemoryAllocSize()));
  }

  return l;
}

static void LuxCore_KernelCacheFill1() {
  KernelCacheFill(std::make_unique<Properties>());
}

static void LuxCore_KernelCacheFill2(PropertiesRPtr config) {
  KernelCacheFill(config);
}

// The C++ KernelCacheFill() progress handler is a plain function pointer, so
// the Python callable has to live in a static trampoline target. Fills are
// serialized by the mutex (only one progress handler can be live at a time).
// The callback object is intentionally leaked: static destruction would
// decref it after interpreter shutdown.
static std::mutex kernelCacheFillProgressMutex;
static py::object *kernelCacheFillProgressCallback = new py::object();

static void KernelCacheFillProgressTrampoline(
		const size_t index, const size_t count) {
	py::gil_scoped_acquire gil;
	try {
		(*kernelCacheFillProgressCallback)(index, count);
	} catch (py::error_already_set &e) {
		e.discard_as_unraisable("KernelCacheFill progress callback");
	}
}

// Bound with call_guard<gil_scoped_release>: the whole body runs without the
// GIL, so every Python object access is wrapped in gil_scoped_acquire.
static void LuxCore_KernelCacheFill3(
		PropertiesRPtr config, const py::object &progress) {
	std::lock_guard<std::mutex> lock(kernelCacheFillProgressMutex);
	if (progress.is_none()) {  // Py_None pointer compare, safe without GIL
		KernelCacheFill(config);
		return;
	}

	{
		py::gil_scoped_acquire gil;
		*kernelCacheFillProgressCallback = progress;
	}
	try {
		KernelCacheFill(config, &KernelCacheFillProgressTrampoline);
	} catch (...) {
		py::gil_scoped_acquire gil;
		*kernelCacheFillProgressCallback = py::none();
		throw;
	}
	{
		py::gil_scoped_acquire gil;
		*kernelCacheFillProgressCallback = py::none();
	}
}

//------------------------------------------------------------------------------
// OpenVDB helper functions
//------------------------------------------------------------------------------
py::list GetOpenVDBGridNames(const std::string &filePathStr) {
  slg::EnsureOpenVDBGridTypes();
  py::list gridNames;

  openvdb::io::File file(filePathStr);
  file.open();
  for (auto i = file.beginName(); i != file.endName(); ++i)
    gridNames.append(*i);

  file.close();
  return gridNames;
}


py::tuple GetOpenVDBGridInfo(const std::string &filePathStr, const std::string &gridName, const bool activeBounds) {
  slg::EnsureOpenVDBGridTypes();
  py::list bBox;
  py::list bBox_w;
  py::list trans_matrix;
  py::list BlenderMetadata;
  openvdb::io::File file(filePathStr);
  
  file.open();
  openvdb::MetaMap::Ptr ovdbMetaMap = file.getMetadata();

  std::string creator = "";
  try {
    creator = ovdbMetaMap->metaValue<std::string>("creator");
  } catch (openvdb::LookupError &e) {
    std::cout << "No creator file meta data found in OpenVDB file " + filePathStr << std::endl;
  };

  openvdb::GridBase::Ptr ovdbGrid = file.readGridMetadata(gridName);  

  //const openvdb::Vec3i bbox_min = ovdbGrid->metaValue<openvdb::Vec3i>("file_bbox_min");
  //const openvdb::Vec3i bbox_max = ovdbGrid->metaValue<openvdb::Vec3i>("file_bbox_max");

  const openvdb::math::Transform &transform = ovdbGrid->transform();
  openvdb::math::Mat4f matrix = transform.baseMap()->getAffineMap()->getMat4();

  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      trans_matrix.append(matrix(col, row));
    }
  }

  // Read the grid from the file
  ovdbGrid = file.readGrid(gridName);

  openvdb::CoordBBox coordbbox;
  if (activeBounds)
    coordbbox = ovdbGrid->evalActiveVoxelBoundingBox();
  else
    ovdbGrid->baseTree().evalLeafBoundingBox(coordbbox);
  openvdb::BBoxd bbox_world = ovdbGrid->transform().indexToWorld(coordbbox);

  bBox.append(coordbbox.min()[0]);
  bBox.append(coordbbox.min()[1]);
  bBox.append(coordbbox.min()[2]);

  bBox.append(coordbbox.max()[0]);
  bBox.append(coordbbox.max()[1]);
  bBox.append(coordbbox.max()[2]);

  bBox_w.append(bbox_world.min().x());
  bBox_w.append(bbox_world.min().y());
  bBox_w.append(bbox_world.min().z());

  bBox_w.append(bbox_world.max().x());
  bBox_w.append(bbox_world.max().y());
  bBox_w.append(bbox_world.max().z());


  if (creator == "Blender/Smoke") {
    py::list min_bbox_list;
    py::list max_bbox_list;
    py::list res_list;
    py::list minres_list;
    py::list maxres_list;
    py::list baseres_list;
    py::list obmat_list;
    py::list obj_shift_f_list;

    openvdb::Vec3s min_bbox = ovdbMetaMap->metaValue<openvdb::Vec3s>("blender/smoke/min_bbox");
    openvdb::Vec3s max_bbox = ovdbMetaMap->metaValue<openvdb::Vec3s>("blender/smoke/max_bbox");
    openvdb::Vec3i res = ovdbMetaMap->metaValue<openvdb::Vec3i>("blender/smoke/resolution");

    //adaptive domain settings
    openvdb::Vec3i minres = ovdbMetaMap->metaValue<openvdb::Vec3i>("blender/smoke/min_resolution");
    openvdb::Vec3i maxres = ovdbMetaMap->metaValue<openvdb::Vec3i>("blender/smoke/max_resolution");

    openvdb::Vec3i base_res = ovdbMetaMap->metaValue<openvdb::Vec3i>("blender/smoke/base_resolution");

    openvdb::Mat4s obmat = ovdbMetaMap->metaValue<openvdb::Mat4s>("blender/smoke/obmat");
    openvdb::Vec3s obj_shift_f = ovdbMetaMap->metaValue<openvdb::Vec3s>("blender/smoke/obj_shift_f");

    min_bbox_list.append(min_bbox[0]);
    min_bbox_list.append(min_bbox[1]);
    min_bbox_list.append(min_bbox[2]);

    max_bbox_list.append(max_bbox[0]);
    max_bbox_list.append(max_bbox[1]);
    max_bbox_list.append(max_bbox[2]);

    res_list.append(res[0]);
    res_list.append(res[1]);
    res_list.append(res[2]);

    minres_list.append(minres[0]);
    minres_list.append(minres[1]);
    minres_list.append(minres[2]);

    maxres_list.append(maxres[0]);
    maxres_list.append(maxres[1]);
    maxres_list.append(maxres[2]);

    baseres_list.append(base_res[0]);
    baseres_list.append(base_res[1]);
    baseres_list.append(base_res[2]);

    obmat_list.append(obmat[0][0]);
    obmat_list.append(obmat[0][1]);
    obmat_list.append(obmat[0][2]);
    obmat_list.append(obmat[0][3]);

    obmat_list.append(obmat[1][0]);
    obmat_list.append(obmat[1][1]);
    obmat_list.append(obmat[1][2]);
    obmat_list.append(obmat[1][3]);

    obmat_list.append(obmat[2][0]);
    obmat_list.append(obmat[2][1]);
    obmat_list.append(obmat[2][2]);
    obmat_list.append(obmat[2][3]);

    obmat_list.append(obmat[3][0]);
    obmat_list.append(obmat[3][1]);
    obmat_list.append(obmat[3][2]);
    obmat_list.append(obmat[3][3]);

    obj_shift_f_list.append(obj_shift_f[0]);
    obj_shift_f_list.append(obj_shift_f[1]);
    obj_shift_f_list.append(obj_shift_f[2]);

    BlenderMetadata.append(min_bbox_list);
    BlenderMetadata.append(max_bbox_list);
    BlenderMetadata.append(res_list);
    BlenderMetadata.append(minres_list);
    BlenderMetadata.append(maxres_list);
    BlenderMetadata.append(baseres_list);
    BlenderMetadata.append(obmat_list);
    BlenderMetadata.append(obj_shift_f_list);
  };

  file.close();

  return py::make_tuple(creator, bBox, bBox_w, trans_matrix, ovdbGrid->valueType(), BlenderMetadata);
}

//------------------------------------------------------------------------------
// Glue for Property class
//------------------------------------------------------------------------------

static py::list Property_GetBlobByIndex(PropertyRPtr prop, const size_t i) {
  const luxrays::Blob &blob = prop->Get<const luxrays::Blob &>(i);
  const char *data = blob.GetData();
  const size_t size = blob.GetSize();

  py::list l;
  for (size_t i = 0; i < size; ++i)
    l.append((int)data[i]);

  return l;
}

static py::list Property_Get(PropertyRPtr prop) {
  py::list l;
  for (u_int i = 0; i < prop->GetSize(); ++i) {
    const auto dataType = prop->GetValueType(i);

    switch (dataType) {
      case luxrays::PropertyValue::BOOL_VAL:
        l.append(prop->Get<bool>(i));
        break;
      case luxrays::PropertyValue::INT_VAL:
      case luxrays::PropertyValue::LONGLONG_VAL:
        l.append(prop->Get<long long>(i));
        break;
      case luxrays::PropertyValue::DOUBLE_VAL:
        l.append(prop->Get<double>(i));
        break;
      case luxrays::PropertyValue::STRING_VAL:
        l.append(prop->Get<std::string>(i));
        break;
      case luxrays::PropertyValue::BLOB_VAL:
        l.append(Property_GetBlobByIndex(prop, i));
        break;
      default:
        throw std::runtime_error("Unsupported data type in list extraction of a Property: " + prop->GetName());
    }
  }

  return l;
}

static py::list Property_GetBools(PropertyRPtr prop) {
  py::list l;
  for (u_int i = 0; i < prop->GetSize(); ++i)
    l.append(prop->Get<bool>(i));
  return l;
}

static py::list Property_GetInts(PropertyRPtr prop) {
  py::list l;
  for (u_int i = 0; i < prop->GetSize(); ++i)
    l.append(prop->Get<long long>(i));
  return l;
}

static py::list Property_GetFloats(PropertyRPtr prop) {
  py::list l;
  for (u_int i = 0; i < prop->GetSize(); ++i)
    l.append(prop->Get<double>(i));
  return l;
}

static py::list Property_GetStrings(PropertyRPtr prop) {
  py::list l;
  for (u_int i = 0; i < prop->GetSize(); ++i)
    l.append(prop->Get<std::string>(i));
  return l;
}

static py::list Property_GetBlobs(PropertyRPtr prop) {
  py::list l;
  for (u_int i = 0; i < prop->GetSize(); ++i)
    l.append(Property_GetBlobByIndex(prop, i));
  return l;
}

static bool Property_GetBool(PropertyRPtr prop) {
  return prop->Get<bool>(0);
}

static long long Property_GetInt(PropertyRPtr prop) {
  return prop->Get<long long>(0);
}

static unsigned long long Property_GetUnsignedLongLong(PropertyRPtr prop) {
  return prop->Get<unsigned long long>(0);
}

static double Property_GetFloat(PropertyRPtr prop) {
  return prop->Get<double>(0);
}

static std::string Property_GetString(PropertyRPtr prop) {
  return prop->Get<std::string>(0);
}

static py::list Property_GetBlob(PropertyRPtr prop) {
  return Property_GetBlobByIndex(prop, 0);
}

static PropertyRPtr Property_Add(PropertyRPtr prop, const py::list &l) {
  const py::ssize_t size = len(l);
  for (py::ssize_t i = 0; i < size; ++i) {
    const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
    const py::object obj = l[i];

    if (objType == "bool") {
      const bool v = py::cast<bool>(obj);
      prop->Add(v);
    } else if (objType == "int") {
      const long long v = py::cast<long long>(obj);
      prop->Add(v);
    } else if (objType == "float") {
      const double v = py::cast<double>(obj);
      prop->Add(v);
    } else if (objType == "str") {
      const std::string v = py::cast<std::string>(obj);
      prop->Add(v);
    } else if (objType == "list") {
      const py::list ol = py::cast<py::list>(obj);

      const py::ssize_t os = len(ol);

      std::vector<char> data(os);
      for (py::ssize_t i = 0; i < os; ++i)
        data[i] = py::cast<int>(ol[i]);

      prop->Add(std::make_shared<luxrays::Blob>(&data[0], os));
    } else if (PyObject_CheckBuffer(obj.ptr())) {
      Py_buffer view;
      if (!PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE)) {
        const char *buffer = (char *)view.buf;
        const size_t size = (size_t)view.len;

        auto blob = std::make_shared<luxrays::Blob>(buffer, size);
        prop->Add(blob);

        PyBuffer_Release(&view);
      } else
        throw std::runtime_error("Unable to get a data view in Property.Add() method: " + objType);
    } else
      throw std::runtime_error("Unsupported data type included in Property.Add() method list: " + objType);
  }

  return prop;
}

static PropertyRPtr Property_AddAllBool(PropertyRPtr prop,
    const py::object &obj) {
  std::vector<bool> v;
  GetArray<bool>(obj, v);

  for (auto e : v)
    prop->Add<bool>(e);

  return prop;
}

static PropertyRPtr Property_AddAllInt(PropertyRPtr prop,
    const py::object &obj) {
  std::vector<long long> v;
  GetArray<long long>(obj, v);

  for (auto e : v)
    prop->Add<long long>(e);

  return prop;
}

static PropertyRPtr Property_AddAllUnsignedLongLong(PropertyRPtr prop,
    const py::object &obj) {
  std::vector<unsigned long long> v;
  GetArray<unsigned long long>(obj, v);

  for (auto e : v)
    prop->Add<unsigned long long>(e);

  return prop;
}

static PropertyRPtr Property_AddAllFloat(PropertyRPtr prop,
    const py::object &obj) {
  std::vector<float> v;
  GetArray<float>(obj, v);

  for (auto e : v)
    prop->Add<float>(e);

  return prop;
}

static PropertyRPtr Property_AddAllBoolStride(PropertyRPtr prop,
    const py::object &obj, const size_t width, const size_t stride) {
  std::vector<bool> v;
  GetArray<bool>(obj, v, width, stride);

  for (auto e : v)
    prop->Add<bool>(e);

  return prop;
}

static PropertyRPtr Property_AddAllIntStride(PropertyRPtr prop,
    const py::object &obj, const size_t width, const size_t stride) {
  std::vector<long long> v;
  GetArray<long long>(obj, v, width, stride);

  for (auto e : v)
    prop->Add<long long>(e);

  return prop;
}

static PropertyRPtr Property_AddAllUnsignedLongLongStride(PropertyRPtr prop,
    const py::object &obj, const size_t width, const size_t stride) {
  std::vector<unsigned long long> v;
  GetArray<unsigned long long>(obj, v, width, stride);

  for (auto e : v)
    prop->Add<unsigned long long>(e);

  return prop;
}

static PropertyRPtr Property_AddAllFloatStride(PropertyRPtr prop,
    const py::object &obj, const size_t width, const size_t stride) {
  std::vector<float> v;
  GetArray<float>(obj, v, width, stride);

  for (auto e : v)
    prop->Add<float>(e);

  return prop;
}

static PropertyRPtr Property_Set(PropertyRPtr prop, const size_t i,
    const py::object &obj) {
  const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));

  if (objType == "bool") {
    const bool v = py::cast<bool>(obj);
    prop->Set(i, v);
  } else if (objType == "int") {
    const long long v = py::cast<long long>(obj);
    prop->Set(i, v);
  } else if (objType == "float") {
    const double v = py::cast<double>(obj);
    prop->Set(i, v);
  } else if (objType == "str") {
    const std::string v = py::cast<std::string>(obj);
    prop->Set(i, v);
  } else if (objType == "list") {
    const py::list ol = py::cast<py::list>(obj);

    const py::ssize_t os = len(ol);

    std::vector<char> data(os);
    for (py::ssize_t i = 0; i < os; ++i)
      data[i] = py::cast<int>(ol[i]);

    prop->Set(i, std::make_shared<luxrays::Blob>(&data[0], os));
  } else if (PyObject_CheckBuffer(obj.ptr())) {
    Py_buffer view;
    if (!PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE)) {
      const char *buffer = (char *)view.buf;
      const size_t size = (size_t)view.len;

      auto blob = std::make_shared<luxrays::Blob>(buffer, size);
      prop->Set(i, blob);

      PyBuffer_Release(&view);
    } else
      throw std::runtime_error("Unable to get a data view in Property->Set() method: " + objType);
  } else
    throw std::runtime_error("Unsupported data type used for Property->Set() method: " + objType);

  return prop;
}

static PropertyRPtr Property_Set(PropertyRPtr prop, const py::list &l) {
  const py::ssize_t size = len(l);
  for (py::ssize_t i = 0; i < size; ++i) {
    const py::object obj = l[i];
    Property_Set(prop, i, obj);
  }

  return prop;
}

static PropertyUPtr Property_InitWithList(const py::str &name, const py::list &l) {
  auto prop = std::make_unique<luxrays::Property>(py::cast<std::string>(name));

  Property_Add(prop, l);

  return prop;
}

//------------------------------------------------------------------------------
// Glue for Properties class
//------------------------------------------------------------------------------


static luxrays::Property Properties_GetWithDefaultValues(luxrays::PropertiesRPtr props,
    const std::string &name, const py::list &l) {
  luxrays::PropertyValues values;

  const py::ssize_t size = len(l);
  for (py::ssize_t i = 0; i < size; ++i) {
    const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));

    if (objType == "bool") {
      const bool v = py::cast<bool>(l[i]);
      values.push_back(v);
    } else if (objType == "int") {
      const long long v = py::cast<long long>(l[i]);
      values.push_back(v);
    } else if (objType == "float") {
      const double v = py::cast<double>(l[i]);
      values.push_back(v);
    } else if (objType == "str") {
      const std::string v = py::cast<std::string>(l[i]);
      values.push_back(v);
    } else
      throw std::runtime_error("Unsupported data type included in Properties Get with default method: " + objType);
  }

  return props->Get(luxrays::Property(name, values));
}


//------------------------------------------------------------------------------
// Glue for Film class
//------------------------------------------------------------------------------

// Blender bgl.Buffer definition

typedef struct {
  PyObject_VAR_HEAD
  PyObject *parent;

  int type;    /* GL_BYTE, GL_SHORT, GL_INT, GL_FLOAT */
  int ndimensions;
  int *dimensions;

  union {
    char *asbyte;
    short *asshort;
    int *asint;
    float *asfloat;
    double *asdouble;

    void *asvoid;
  } buf;
} BGLBuffer;

//------------------------------------------------------------------------------
// File GetOutput() related functions
//------------------------------------------------------------------------------

static void Film_GetOutputFloat1(
	luxcore::detail::FilmImpl& film,
	const Film::FilmOutputType type,
    py::object &obj,
	const size_t index,
	const bool executeImagePipeline
) {
  const size_t outputSize = film.GetOutputSize(type) * sizeof(float);

  if (PyObject_CheckBuffer(obj.ptr())) {
    Py_buffer view;
    if (!PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE)) {
      if ((size_t)view.len >= outputSize) {
        if(!film.HasOutput(type)) {
          const std::string errorMsg = "Film Output not available: " + luxrays::ToString(type);
          PyBuffer_Release(&view);
          throw std::runtime_error(errorMsg);
        }

        float *buffer = (float *)view.buf;

        {
          // GetOutput runs a full film download + the image pipeline: release
          // the GIL so viewport callers can run it on a worker thread without
          // freezing Blender's UI thread. The capacity-checked overload fuses
          // the buffer-size validation with the write under filmMutex, so a
          // session film resize landing in between can not overflow the view.
          py::gil_scoped_release release;
          film.GetOutputFloat(type, buffer, index, executeImagePipeline, (size_t)view.len);
        }

        PyBuffer_Release(&view);
      } else {
        const std::string errorMsg = "Not enough space in the buffer of Film.GetOutputFloat() method: " +
            luxrays::ToString(view.len) + " instead of " + luxrays::ToString(outputSize);
        PyBuffer_Release(&view);

        throw std::runtime_error(errorMsg);
      }
    } else {
      const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Film.GetOutputFloat() method: " + objType);
    }
  } else {
    const PyObject *pyObj = obj.ptr();
    const PyTypeObject *pyTypeObj = Py_TYPE(pyObj);

    // Check if it is a Blender bgl.Buffer object
    if (strcmp(pyTypeObj->tp_name, "bgl.Buffer") == 0) {
      // This is a special path for optimizing Blender preview
      const BGLBuffer *bglBuffer = (BGLBuffer *)pyObj;

      // A safety check for buffer type and size
      // 0x1406 is the value of GL_FLOAT
      if (bglBuffer->type == 0x1406) {
        if (bglBuffer->ndimensions == 1) {
          if (bglBuffer->dimensions[0] * sizeof(float) >= outputSize) {
            if(!film.HasOutput(type)) {
              throw std::runtime_error("Film Output not available: " + luxrays::ToString(type));
            }

            {
              py::gil_scoped_release release;
              film.GetOutputFloat(type, bglBuffer->buf.asfloat, index, executeImagePipeline,
                  bglBuffer->dimensions[0] * sizeof(float));
            }
          } else
            throw std::runtime_error("Not enough space in the Blender bgl.Buffer of Film.GetOutputFloat() method: " +
                luxrays::ToString(bglBuffer->dimensions[0] * sizeof(float)) + " instead of " + luxrays::ToString(outputSize));
        } else
          throw std::runtime_error("A Blender bgl.Buffer has the wrong dimension in Film.GetOutputFloat(): " + luxrays::ToString(bglBuffer->ndimensions));
      } else
        throw std::runtime_error("A Blender bgl.Buffer has the wrong type in Film.GetOutputFloat(): " + luxrays::ToString(bglBuffer->type));
    } else {
      const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unsupported data type in Film.GetOutputFloat(): " + objType);
    }
  }
}

// The buffer lease and its format/contiguity validation survive native exceptions.
static PropertiesUPtr Film_GetOutputFloatSnapshot(FilmImpl &film,
        const Film::FilmOutputType type, py::object &obj,
        const size_t index, const bool executeImagePipeline) {
    Py_buffer view{};
    if (PyObject_GetBuffer(obj.ptr(), &view,
            PyBUF_C_CONTIGUOUS | PyBUF_FORMAT | PyBUF_WRITABLE) != 0)
        throw py::error_already_set();
    struct ReleaseBuffer { Py_buffer *view; ~ReleaseBuffer() { PyBuffer_Release(view); } } lease{&view};
    if (view.itemsize != sizeof(float) || !view.format || strcmp(view.format, "f") != 0 ||
            reinterpret_cast<uintptr_t>(view.buf) % alignof(float) != 0 || view.len < 0)
        throw std::runtime_error("GetOutputFloatSnapshot requires aligned writable C-contiguous native float32");
    PropertiesUPtr result;
    {
        py::gil_scoped_release release;
        result = film.GetOutputFloatSnapshot(type, static_cast<float *>(view.buf),
                index, executeImagePipeline, static_cast<size_t>(view.len));
    }
    return result;
}

static void Film_GetOutputFloat2(
	FilmImpl & film,
	const Film::FilmOutputType type,
    py::object &obj
) {
  Film_GetOutputFloat1(film, type, obj, 0, true);
}

static void Film_GetOutputFloat3(
    FilmImpl & film,
    const Film::FilmOutputType type,
    py::object &obj,
    const size_t index
) {
  Film_GetOutputFloat1(film, type, obj, index, true);
}

static void Film_GetOutputUInt1(
    FilmImpl & film,
    const Film::FilmOutputType type,
    py::object &obj,
    const size_t index,
    const bool executeImagePipeline) {
  if (PyObject_CheckBuffer(obj.ptr())) {
    Py_buffer view;
    if (!PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE)) {
      if ((size_t)view.len >= film.GetOutputSize(type) * sizeof(u_int)) {
        if(!film.HasOutput(type)) {
          const std::string errorMsg = "Film Output not available: " + luxrays::ToString(type);
          PyBuffer_Release(&view);
          throw std::runtime_error(errorMsg);
        }

        u_int *buffer = (u_int *)view.buf;

        {
          py::gil_scoped_release release;
          film.GetOutputUInt(type, buffer, index, executeImagePipeline, (size_t)view.len);
        }

        PyBuffer_Release(&view);
      } else {
        const std::string errorMsg = "Not enough space in the buffer of Film.GetOutputUInt() method: " +
            luxrays::ToString(view.len) + " instead of " + luxrays::ToString(film.GetOutputSize(type) * sizeof(u_int));
        PyBuffer_Release(&view);

        throw std::runtime_error(errorMsg);
      }
    } else {
      const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Film.GetOutputUInt() method: " + objType);
    }
  } else {
    const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Unsupported data type in Film.GetOutputUInt() method: " + objType);
  }
}

static void Film_GetOutputUInt2(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj) {
  Film_GetOutputUInt1(film, type, obj, 0, true);
}

static void Film_GetOutputUInt3(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj, const size_t index) {
  Film_GetOutputUInt1(film, type, obj, index, true);
}

//------------------------------------------------------------------------------
// File UpdateOutput() related functions
//------------------------------------------------------------------------------

static void Film_UpdateOutputFloat1(
    FilmImpl & film,
    const Film::FilmOutputType type,
    py::object &obj,
    const size_t index,
    const bool executeImagePipeline) {
  const size_t outputSize = film.GetOutputSize(type) * sizeof(float);

  if (PyObject_CheckBuffer(obj.ptr())) {
    Py_buffer view;
    if (!PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE)) {
      if ((size_t)view.len >= outputSize) {
        if(!film.HasOutput(type)) {
          const std::string errorMsg = "Film Output not available: " + luxrays::ToString(type);
          PyBuffer_Release(&view);
          throw std::runtime_error(errorMsg);
        }

        float *buffer = (float *)view.buf;

        {
          py::gil_scoped_release release;
          film.UpdateOutput<float>(type, buffer, index, executeImagePipeline);
        }

        PyBuffer_Release(&view);
      } else {
        const std::string errorMsg = "Not enough space in the buffer of Film.UpdateOutputFloat() method: " +
            luxrays::ToString(view.len) + " instead of " + luxrays::ToString(outputSize);
        PyBuffer_Release(&view);

        throw std::runtime_error(errorMsg);
      }
    } else {
      const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Film.UpdateOutputFloat() method: " + objType);
    }
  } else {
    const PyObject *pyObj = obj.ptr();
    const PyTypeObject *pyTypeObj = Py_TYPE(pyObj);

    // Check if it is a Blender bgl.Buffer object
    if (strcmp(pyTypeObj->tp_name, "bgl.Buffer") == 0) {
      // This is a special path for optimizing Blender preview
      const BGLBuffer *bglBuffer = (BGLBuffer *)pyObj;

      // A safety check for buffer type and size
      // 0x1406 is the value of GL_FLOAT
      if (bglBuffer->type == 0x1406) {
        if (bglBuffer->ndimensions == 1) {
          if (bglBuffer->dimensions[0] * sizeof(float) >= outputSize) {
            if(!film.HasOutput(type)) {
              throw std::runtime_error("Film Output not available: " + luxrays::ToString(type));
            }

            {
              py::gil_scoped_release release;
              film.UpdateOutput<float>(type, bglBuffer->buf.asfloat, index, executeImagePipeline);
            }
          } else
            throw std::runtime_error("Not enough space in the Blender bgl.Buffer of Film.UpdateOutputFloat() method: " +
                luxrays::ToString(bglBuffer->dimensions[0] * sizeof(float)) + " instead of " + luxrays::ToString(outputSize));
        } else
          throw std::runtime_error("A Blender bgl.Buffer has the wrong dimension in Film.GetOutputFloat(): " + luxrays::ToString(bglBuffer->ndimensions));
      } else
        throw std::runtime_error("A Blender bgl.Buffer has the wrong type in Film.GetOutputFloat(): " + luxrays::ToString(bglBuffer->type));
    } else {
      const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unsupported data type in Film.GetOutputFloat(): " + objType);
    }
  }
}

static void Film_UpdateOutputFloat2(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj) {
  Film_UpdateOutputFloat1(film, type, obj, 0, false);
}

static void Film_UpdateOutputFloat3(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj, const size_t index) {
  Film_UpdateOutputFloat1(film, type, obj, index, false);
}

static void Film_UpdateOutputUInt1(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj, const size_t index, const bool executeImagePipeline) {
  throw std::runtime_error("Film Output not available: " + luxrays::ToString(type));
}

static void Film_UpdateOutputUInt2(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj) {
  Film_UpdateOutputUInt1(film, type, obj, 0, false);
}

static void Film_UpdateOutputUInt3(FilmImpl & film, const Film::FilmOutputType type,
    py::object &obj, const size_t index) {
  Film_UpdateOutputUInt1(film, type, obj, index, false);
}

//------------------------------------------------------------------------------

static void Film_AddFilm1(
	std::unique_ptr<luxcore::detail::FilmImpl> film,
	std::unique_ptr<luxcore::detail::FilmImpl> srcFilm
) {
  film->AddFilm(*srcFilm);
}

static void Film_AddFilm2(
	std::unique_ptr<luxcore::detail::FilmImpl> film,
	std::unique_ptr<luxcore::detail::FilmImpl> srcFilm,
    const size_t srcOffsetX, const size_t srcOffsetY,
    const size_t srcWidth, const size_t srcHeight,
    const size_t dstOffsetX, const size_t dstOffsetY
) {
	film->AddFilm(
		*srcFilm, srcOffsetX, srcOffsetY, srcWidth,  srcHeight, dstOffsetX,  dstOffsetY
	);
}

static float Film_GetFilmY1(FilmImpl & film) {
  return film.GetFilmY();
}

static float Film_GetFilmY2(FilmImpl & film, const size_t imagePipelineIndex) {
  return film.GetFilmY(imagePipelineIndex);
}

static void Film_ApplyOIDN(
	FilmImpl & film,
	const size_t imagePipelineIndex
) {
	film.ApplyOIDN(imagePipelineIndex);
}

//------------------------------------------------------------------------------
// Glue for Camera class
//------------------------------------------------------------------------------

static void Camera_Translate(luxcore::detail::CameraImpl *camera, const py::tuple t) {
  camera->Translate(py::cast<float>(t[0]), py::cast<float>(t[1]), py::cast<float>(t[2]));
}

static void Camera_Rotate(luxcore::detail::CameraImpl *camera, const float angle, const py::tuple axis) {
  camera->Rotate(angle, py::cast<float>(axis[0]), py::cast<float>(axis[1]), py::cast<float>(axis[2]));
}

//------------------------------------------------------------------------------
// Glue for Scene class
//------------------------------------------------------------------------------

static luxcore::detail::CameraImpl & Scene_GetCamera(const SceneImplPtr & scene) {
  return dynamic_cast<luxcore::detail::CameraImpl &>(scene->GetCamera());
}


static void Scene_DefineImageMap(
	const SceneImplPtr & scene,
	const std::string &imgMapName,
    py::object &obj,
	const float gamma,
    const size_t channels,
	const size_t width,
	const size_t height
) {
  if (PyObject_CheckBuffer(obj.ptr())) {
    Py_buffer view;
    if (!PyObject_GetBuffer(obj.ptr(), &view, PyBUF_SIMPLE)) {
      if ((size_t)view.len >= width * height * channels * sizeof(float)) {
        float *buffer = (float *)view.buf;
        {
          py::gil_scoped_release release;
          scene->DefineImageMap(imgMapName, buffer, gamma, channels,
              width, height, Scene::DEFAULT, Scene::REPEAT);
        }

        PyBuffer_Release(&view);
      } else {
        const std::string errorMsg = "Not enough space in the buffer of Scene.DefineImageMap() method: " +
            luxrays::ToString(view.len) + " instead of " + luxrays::ToString(width * height * channels * sizeof(float));
        PyBuffer_Release(&view);

        throw std::runtime_error(errorMsg);
      }
    } else {
      const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Scene.DefineImageMap() method: " + objType);
    }
  }  else {
    const std::string objType = py::cast<std::string>((obj.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Unsupported data type Scene.DefineImageMap() method: " + objType);
  }
}

// Helpers for mesh property translation
template<typename T, size_t N>
static luxrays::ExtMeshProp<T>::Layer translateLayer(
	const py::object& obj,
	const std::string objname
) {
	// Require non-empty input
	if (obj.is_none()) return nullptr;

	// Check input type
	if (not py::isinstance<py::list>(obj)) {
		const std::string objType =
			py::cast<std::string>((obj.attr("__class__")).attr("__name__"));

		throw std::runtime_error(
			"Wrong data type for the list of "
			+ objname
			+ " of method Scene.DefineMesh(): "
			+ objType
		);
	}

	const py::list &l = py::cast<py::list>(obj);
	const py::ssize_t size = len(l);

	// Process
	auto out = std::make_shared<T[]>(size);
	for (py::ssize_t i = 0; i < size; ++i) {

		// Check list member consistency
		if (not py::isinstance<py::tuple>(l[i])) {
		  const std::string objType =
			py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
		  throw std::runtime_error(
			"Wrong data type in the list of "
			+ objname
			+ "UVs of method Scene.DefineMesh() at position "
			+ luxrays::ToString(i) + ": " + objType
		  );
		}

		// Process element
		const py::tuple &t = py::cast<py::tuple>(l[i]);
		constexpr auto cast = []<size_t... Is>(
		  const py::tuple& t, size_t i, std::index_sequence<Is...>
		) {
		  return T{py::cast<float>(t[Is])...};
		};
		out[i] = cast(t, i, std::make_index_sequence<N>{});
	}

	return out;

}  // translateLayer


template<typename T, size_t N>
static std::optional<luxrays::ExtMeshProp<T>> translateProp(
	const py::object& obj,
	const std::string objname
) {
	// Require non-empty input
	if (obj.is_none()) return std::nullopt;


    if (not py::isinstance<py::list>(obj)) {
		const std::string objType =
			py::cast<std::string>((obj.attr("__class__")).attr("__name__"));

		throw std::runtime_error(
			"Wrong data type for the list of "
			+ objname
			+ " of method Scene.DefineMesh(): "
			+ objType
		);
	}

	// Cast input
	const py::list& layers = py::cast<py::list>(obj);
	const py::ssize_t size = std::min(len(layers), size_t(LC_MESH_MAX_DATA_COUNT));;

	// Declare output
	luxrays::ExtMeshProp<T> out;

	// Process layers
    for (py::ssize_t j=0; j < size; ++j) {
		auto layer = layers[j];

		if (layer.is_none()) continue;

		// Cast layer to list
        if (not py::isinstance<py::list>(layer)) {
			const std::string objType =
				py::cast<std::string>((layer.attr("__class__")).attr("__name__"));
			throw std::runtime_error(
				"Wrong data type for the list of " + objname
				+ " in the layer " + std::to_string(j)
				+ " of method Scene.DefineMeshExt(): " + objType
			);
		}
		const py::list &l = py::cast<py::list>(layer);

		// Process layer
		const py::ssize_t size = len(l);
		out[j] = std::make_shared<T[]>(size);
		for (py::ssize_t i = 0; i < size; ++i) {
			// Require element to be a tuple
			if(not py::isinstance<py::tuple>(l[i])) {
                const std::string objType =
					py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
                throw std::runtime_error(
					"Wrong data type in the list of " + objname + " colors of method "
					+ "Scene.DefineMeshExt() at position " + luxrays::ToString(i)
					+": " + objType
				);
			}

			// Process element
			const py::tuple &t = py::cast<py::tuple>(l[i]);
			constexpr auto cast = []<size_t... Is>(
				const py::tuple& t, size_t i, std::index_sequence<Is...>
			) {
				return T{py::cast<float>(t[Is])...};
			};
			out[j][i] = cast(t, i, std::make_index_sequence<N>{});
		}
	}

	return out;
}


static void Scene_DefineMesh1(
	const SceneImplPtr & scene,
	const std::string &meshName,
    const py::object &p, const py::object &vi,
    const py::object &n, const py::object &uv,
    const py::object &cols, const py::object &alphas,
    const py::object &transformation, const py::object &generatedTransformation) {
  // NOTE: I would like to use boost::scoped_array but
  // some guy has decided that boost::scoped_array must not have
  // a release() method for some ideological reason...

  // Translate all vertices
  long plyNbVerts;
  luxrays::VertexBuffer points;
  if (py::isinstance<py::list>(p)) {
    const py::list &l = py::cast<py::list>(p);
    const py::ssize_t size = len(l);
    plyNbVerts = size;

    points.Allocate(size);
    for (py::ssize_t i = 0; i < size; ++i) {
      if (py::isinstance<py::tuple>(l[i])) {
        const py::tuple &t = py::cast<py::tuple>(l[i]);
        points[i] = luxrays::Point(py::cast<float>(t[0]), py::cast<float>(t[1]), py::cast<float>(t[2]));
      } else {
        const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
        throw std::runtime_error("Wrong data type in the list of vertices of method Scene.DefineMesh() at position " + luxrays::ToString(i) +": " + objType);
      }
    }
  } else {
    const std::string objType = py::cast<std::string>((p.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Wrong data type for the list of vertices of method Scene.DefineMesh(): " + objType);
  }

  // Translate all triangles
  // TODO shoud be optimized
  luxrays::TriangleBuffer tris;
  if (py::isinstance<py::list>(vi)) {
    const py::list &l = py::cast<py::list>(vi);
    const py::ssize_t size = len(l);

	tris.Allocate(size);
    for (py::ssize_t i = 0; i < size; ++i) {
      if (py::isinstance<py::tuple>(l[i])) {
        const py::tuple &t = py::cast<py::tuple>(l[i]);
        tris[i] = luxrays::Triangle(
			py::cast<luxrays::Triangle::subtype_t>(t[0]),
			py::cast<luxrays::Triangle::subtype_t>(t[1]),
			py::cast<luxrays::Triangle::subtype_t>(t[2]));
      } else {
        const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
        throw std::runtime_error("Wrong data type in the list of triangles of method Scene.DefineMesh() at position " + luxrays::ToString(i) +": " + objType);
      }
    }
  } else {
    const std::string objType = py::cast<std::string>((vi.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Wrong data type for the list of triangles of method Scene.DefineMesh(): " + objType);
  }

  // Translate all normals
  luxrays::NormalBuffer normals;
  if (!n.is_none()) {
    if(py::isinstance<py::list>(n)) {
      const py::list &l = py::cast<py::list>(n);
      const py::ssize_t size = len(l);

      normals.Allocate(size);
      for (py::ssize_t i = 0; i < size; ++i) {
        if(py::isinstance<py::tuple>(l[i])) {
          const py::tuple &t = py::cast<py::tuple>(l[i]);
          normals[i] = luxrays::Normal(py::cast<float>(t[0]), py::cast<float>(t[1]), py::cast<float>(t[2]));
        } else {
          const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
          throw std::runtime_error("Wrong data type in the list of triangles of method Scene.DefineMesh() at position " + luxrays::ToString(i) +": " + objType);
        }
      }
    } else {
      const std::string objType = py::cast<std::string>((n.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of triangles of method Scene.DefineMesh(): " + objType);
    }
  }



  // Translate all UVs, Colors, Alphas
  auto uvs = translateLayer<luxrays::UV, 2>(uv, "UVs");
  auto colors = translateLayer<luxrays::Spectrum, 3>(cols, "colors");
  auto as = translateLayer<float, 1>(alphas, "alphas");


  auto mesh = std::make_unique<luxrays::ExtTriangleMesh>(
  	std::move(points), std::move(tris), std::move(normals), uvs, colors, as
  );

  // Apply the transformation if required
  if (!transformation.is_none()) {
  float mat[16];
  GetMatrix4x4(transformation, mat);
  mesh->ApplyTransform(luxrays::Transform(luxrays::Matrix4x4(mat).Transpose()));
  }
  if (!generatedTransformation.is_none()) {
    float mat[16];
    GetMatrix4x4(generatedTransformation, mat);
    mesh->SetGeneratedTransformation(luxrays::Matrix4x4(mat).Transpose());
  }

  mesh->SetName(meshName);
  {
    py::gil_scoped_release release;
    scene->DefineMesh(std::move(mesh));
  }
}


static void Scene_DefineMeshExt1(
	const SceneImplPtr & scene,
	const std::string &meshName,
    const py::object &p, const py::object &vi,
    const py::object &n, const py::object &uv,
    const py::object &cols, const py::object &alphas,
    const py::object &transformation, const py::object &generatedTransformation
) {

  // Translate all vertices
  long plyNbVerts;
  luxrays::VertexBuffer points;
  if(py::isinstance<py::list>(p)) {
    const py::list &l = py::cast<py::list>(p);
    const py::ssize_t size = len(l);
    plyNbVerts = size;

    points.Allocate(size);
    for (py::ssize_t i = 0; i < size; ++i) {
      if(py::isinstance<py::tuple>(l[i])) {
        const py::tuple &t = py::cast<py::tuple>(l[i]);
        points[i] = luxrays::Point(py::cast<float>(t[0]), py::cast<float>(t[1]), py::cast<float>(t[2]));
      } else {
        const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
        throw std::runtime_error("Wrong data type in the list of vertices of method Scene.DefineMeshExt() at position " + luxrays::ToString(i) +": " + objType);
      }
    }
  } else {
    const std::string objType = py::cast<std::string>((p.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Wrong data type for the list of vertices of method Scene.DefineMeshExt(): " + objType);
  }

  // Translate all triangles
  long plyNbTris;
  luxrays::TriangleBuffer tris;
  if(py::isinstance<py::list>(vi)) {
    const py::list &l = py::cast<py::list>(vi);
    const py::ssize_t size = len(l);
    plyNbTris = size;

	tris.Allocate(size);
    for (py::ssize_t i = 0; i < size; ++i) {
      if(py::isinstance<py::tuple>(l[i])) {
        const py::tuple &t = py::cast<py::tuple>(l[i]);
        tris[i] = luxrays::Triangle(py::cast<u_int>(t[0]), py::cast<u_int>(t[1]), py::cast<u_int>(t[2]));
      } else {
        const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
        throw std::runtime_error("Wrong data type in the list of triangles of method Scene.DefineMeshExt() at position " + luxrays::ToString(i) +": " + objType);
      }
    }
  } else {
    const std::string objType = py::cast<std::string>((vi.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Wrong data type for the list of triangles of method Scene.DefineMeshExt(): " + objType);
  }

  // Translate all normals
  luxrays::NormalBuffer normals;
  if (!n.is_none()) {
    if(py::isinstance<py::list>(n)) {
      const py::list &l = py::cast<py::list>(n);
      const py::ssize_t size = len(l);

      normals.Allocate(size);
      for (py::ssize_t i = 0; i < size; ++i) {
        if(py::isinstance<py::tuple>(l[i])) {
          const py::tuple &t = py::cast<py::tuple>(l[i]);
          normals[i] = luxrays::Normal(py::cast<float>(t[0]), py::cast<float>(t[1]), py::cast<float>(t[2]));
        } else {
          const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
          throw std::runtime_error("Wrong data type in the list of triangles of method Scene.DefineMeshExt() at position " + luxrays::ToString(i) +": " + objType);
        }
      }
    } else {
      const std::string objType = py::cast<std::string>((n.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of triangles of method Scene.DefineMeshExt(): " + objType);
    }
  }

  // Translate UVs, colors, alphas
  auto uvs = translateProp<luxrays::UV, 2>(uv, "UVs");
  auto colors = translateProp<luxrays::Spectrum, 3>(cols, "colors");
  auto as = translateProp<float, 1>(alphas, "alphas");

	auto mesh = std::make_unique<luxrays::ExtTriangleMesh>(
		std::move(points), std::move(tris), std::move(normals), uvs, colors, as
	);

	// Apply the transformation if required
	if (!transformation.is_none()) {
		float mat[16];
		GetMatrix4x4(transformation, mat);
		mesh->ApplyTransform(luxrays::Transform(luxrays::Matrix4x4(mat).Transpose()));
	}
	if (!generatedTransformation.is_none()) {
		float mat[16];
		GetMatrix4x4(generatedTransformation, mat);
		mesh->SetGeneratedTransformation(luxrays::Matrix4x4(mat).Transpose());
	}

  mesh->SetName(meshName);
  {
    py::gil_scoped_release release;
    scene->DefineMesh(std::move(mesh));
  }
}



// TODO make template and move to anonymous namespace
luxrays::Normal* AllocNormalsBuffer(unsigned int n) {
	return (new luxrays::Normal[n]);
}
luxrays::UV* AllocUVBuffer(unsigned int n) {
	return (new luxrays::UV[n]);
}
luxrays::Spectrum* AllocRGBBuffer(unsigned int n) {
	return (new luxrays::Spectrum[n]);
}
float* AllocAlphaBuffer(unsigned int n) {
	return (new float[n]);
}
template<typename T>
T* AllocBuffer(unsigned n) {
	return (new T[n]);
}

// Helper for DefineMeshExt3
// Allocate internal data structure and copy array inside
template<
	typename S,  // Source type
	typename D,  // Destination type
	D* (*Allocator)(unsigned int),
	size_t stride=3
>
std::tuple<std::unique_ptr<D[]>, u_int> dataCopy(
	py::array_t<S, py::array::c_style> src,
	const std::string& meshName,
	const std::string& propertyName
) {
	auto src_stride = src.shape(1);

	// Remark:
	// All buffers (triangles, points, normals...) are enclosed in smart pointers
	// in order to guarantee that memory is released in case of exception arising
	// during whole process: please keep it so.
	using this_array_t = py::array_t< S, py::array::c_style | py::array::forcecast >;
	auto direct_src = src.template unchecked<2>();

	// Check
	if (src_stride != stride) {
		std::string errorMsg = std::string("Scene.DefineMeshExt: Error - ")
			+ "Mesh '" + meshName + "' / "
			+ "Property '" + std::string(propertyName) + "' - "
			+ "Shape must be [N,"
			+ std::to_string(stride)
			+ "]";
		throw std::runtime_error(errorMsg);
	}

	// Allocate & copy
	u_int count = src.shape(0);
	if (!count) return std::tuple(nullptr, 0);
    auto dest = std::unique_ptr<D[]>(Allocator(count));
	std::memcpy(dest.get(), direct_src.data(0, 0), direct_src.nbytes());

	return std::tuple(std::move(dest), count);
}

// Variant of dataCopy for vertex buffer
// S: Source underlying type; OUT: Output type type
template< typename S,  size_t stride, typename O > 
O dataCopyBuffer(
	py::array_t<S, py::array::c_style> src,
	const std::string& meshName,
	const std::string& propertyName
) {
	auto src_stride = src.shape(1);

	using this_array_t = py::array_t< S, py::array::c_style | py::array::forcecast >;
	auto direct_src = src.template unchecked<2>();

	// Check
	if (src_stride != stride) {
		std::string errorMsg = std::string("Scene.DefineMeshExt: Error - ")
			+ "Mesh '" + meshName + "' / "
			+ "Property '" + std::string(propertyName) + "' - "
			+ "Shape must be [N,"
			+ std::to_string(stride)
			+ "]";
		throw std::runtime_error(errorMsg);
	}

	// Allocate & copy
	if (!src.shape(0)) return O();

	auto count = direct_src.nbytes() / sizeof(S);
	assert(direct_src.nbytes() % sizeof(S) == 0);
	auto in_span = std::span<const S>(direct_src.data(0, 0), count);
	O buf(in_span);

	return buf;
}


// Variant of DataCopy for optional arguments
template<
	typename S,  // Source type
	typename D,  // Destination type
	D* (*Allocator)(unsigned int),
	size_t stride=3
>
std::tuple<std::unique_ptr<D[]>, u_int> dataCopyOptional(
	std::optional<py::array_t<S, py::array::c_style>> src,
	const std::string& meshName,
	const std::string& propertyName
) {
	if (src.has_value()) {
		return dataCopy<S, D, Allocator, stride>(src.value(), meshName, propertyName);
	} else {
		return std::tuple(nullptr, 0);
	}
}


using triangle_underlying_type = std::remove_all_extents<decltype(luxrays::Triangle::v)>::type;

using py_float_array= py::array_t<float, py::array::c_style>;

// Keeps a reference to a numpy array so the memory it owns stays valid
// for as long as a LuxCore buffer adopts it. The reference is dropped
// under the GIL because scene buffers can be released from non-Python
// threads (trash bin, device teardown, session destruction).
static std::shared_ptr<void> numpyKeeper(const py::object &arr) {
	auto *ref = new py::object(py::reinterpret_borrow<py::object>(arr));
	return std::shared_ptr<void>(ref, [](void *p) {
		if (Py_IsInitialized()) {
			py::gil_scoped_acquire gil;
			delete static_cast<py::object*>(p);
		}
		// else: interpreter already finalized — leak the reference
		// rather than decref on a dead interpreter
	});
}

// Variant of dataCopyBuffer that adopts the numpy memory in place
// instead of copying it, removing the transient 2x memory peak of the
// export path. Falls back to a copy when the array is not writable
// (LuxCore may transform vertices in place).
template< typename S,  size_t stride, typename O >
O dataAdoptBuffer(
	py::array_t<S, py::array::c_style> src,
	const std::string& meshName,
	const std::string& propertyName
) {
	auto src_stride = src.shape(1);

	if (src_stride != stride) {
		std::string errorMsg = std::string("Scene.DefineMeshExt: Error - ")
			+ "Mesh '" + meshName + "' / "
			+ "Property '" + std::string(propertyName) + "' - "
			+ "Shape must be [N,"
			+ std::to_string(stride)
			+ "]";
		throw std::runtime_error(errorMsg);
	}

	if (!src.shape(0)) return O();
	if (!src.writeable())
		return dataCopyBuffer<S, stride, O>(src, meshName, propertyName);

	return O::Adopt(src.mutable_data(), src.nbytes(), numpyKeeper(src));
}

template<typename T, size_t N>
static std::optional<luxrays::ExtMeshProp<T>> propCopy(
	const std::optional<std::vector<py_float_array>>& layers,
	const std::string objname,
	const std::string meshName
) {
	if (not layers or layers->empty()) return std::nullopt;

	auto layer_count = layers->size();
	luxrays::ExtMeshProp<T> out;
	if (layer_count > out.GetMaxLayerNumber()) {
		throw std::runtime_error(
			"Too many layers of " + objname + " for method Scene.DefineMesh()"
		);
	}

	for (size_t i = 0; i < layer_count; ++i) {
		auto layer = layers.value()[i];

		if (N == 1) {
			// In caller's logic, alpha_layer shape must be (N,), but in
			// DataCopy logic, it must be (N,1), so we reshape
			pybind11::array::ShapeContainer new_shape({layer.shape(0), 1,});
			layer = layer.reshape(new_shape);
		}
		if (layer.shape(1) != N) {
			throw std::runtime_error(
				std::string("Scene.DefineMeshExt: Error - Mesh '")
				+ meshName + "' / Property '" + objname
				+ "' - Shape must be [N," + std::to_string(N) + "]"
			);
		}
		if (layer.writeable() && layer.shape(0)) {
			// Zero-copy: the LuxCore layer adopts the numpy memory and
			// keeps the array alive through a keeper reference.
			auto keeper = numpyKeeper(layer);
			std::shared_ptr<T[]> sp(keeper,
					reinterpret_cast<T*>(layer.mutable_data()));
			out.SetLayer(i, sp, layer.shape(0));
		} else {
			auto [data, numData] = dataCopy<float, T, &AllocBuffer, N>(
				layer, meshName, objname
			);
			out.SetLayer(i, std::shared_ptr<T[]>(std::move(data)), numData);
		}
	}
	return out;

}


// Define Mesh from Numpy arrays
static void Scene_DefineMeshExt3(
	  const SceneImplPtr & scene,
	  const std::string &meshName,
    const py_float_array p,
	  const py::array_t<triangle_underlying_type, py::array::c_style > tri,
    const std::optional<py_float_array> n,
    const std::optional<std::vector<py_float_array>> uv_layers,
    const std::optional<std::vector<py_float_array>> color_layers,
    const std::optional<std::vector<py_float_array>> alpha_layers,
    const std::optional<py_float_array> transformation,
    const std::optional<py_float_array> generatedTransformation
) {
	// TODO Release GIL when possible

	// Points
	auto points = dataAdoptBuffer< float, 3, luxrays::VertexBuffer > (
		p, meshName, "Points"
	);

	// Triangles
	auto triangles = dataAdoptBuffer< luxrays::Triangle::subtype_t, 3, luxrays::TriangleBuffer> (
		tri, meshName, "Triangles"
	);

  // Normals
  auto normals = n ?
    dataAdoptBuffer< float, 3, luxrays::NormalBuffer > (n.value(), meshName, "Normals") :
    luxrays::NormalBuffer();


	// UV, colors and alphas
	auto meshUVs = propCopy<luxrays::UV, 2>(uv_layers, "UVs", meshName);
	auto meshCols = propCopy<luxrays::Spectrum, 3>(color_layers, "colors", meshName);
	auto meshAlphas = propCopy<float, 1>(alpha_layers, "alphas", meshName);
	assert(not bool(meshUVs) or meshUVs->GetLayerSize() == points.Count());
	assert(not bool(meshCols) or meshCols->GetLayerSize() == points.Count());
	assert(not bool(meshAlphas) or meshAlphas->GetLayerSize() == points.Count());

	// Create Mesh
	auto newMesh = std::make_unique<luxrays::ExtTriangleMesh>(
		std::move(points),
		std::move(triangles),
		std::move(normals),
		meshUVs,
		meshCols,
		meshAlphas
	);
	newMesh->SetName(meshName);

	if (transformation.has_value()) {
		auto transval = transformation.value();
		luxrays::Transform luxtrans{transval.data()};
		newMesh->ApplyTransform(luxtrans);
	}
	if (generatedTransformation.has_value())
		newMesh->SetGeneratedTransformation(luxrays::Matrix4x4(generatedTransformation->data()));


	// Insert mesh into the scene
	{
		py::gil_scoped_release release;
		scene->DefineMesh(std::move(newMesh));
	}
}

static void Scene_SetMeshVertexAOV(
	const SceneImplPtr & scene, const std::string &meshName,
    const size_t index, const py::object &data
) {
	std::vector<float> v;
	GetArray<float>(data, v);

	{
		py::gil_scoped_release release;
		scene->SetMeshVertexAOV(meshName, index, v);
	}
}

static void Scene_SetMeshTriangleAOV(
    const SceneImplPtr & scene,
    const std::string &meshName,
    const size_t index,
    const py::object &data
) {
  std::vector<float> t;
  GetArray<float>(data, t);

  {
    py::gil_scoped_release release;
    scene->SetMeshTriangleAOV(meshName, index, t);
  }
}

static void Scene_SetMeshAppliedTransformation(
    const SceneImplPtr & scene,
    const std::string &meshName,
    const py::object &transformation) {
  float mat[16];
  GetMatrix4x4(transformation, mat);
  {
    py::gil_scoped_release release;
    scene->SetMeshAppliedTransformation(meshName, mat);
  }
}

// Per-vertex deformation motion blur (E9): `times` is a (S,) float array
// of shutter times, `vertsPerStep` a list of S (N,3) float arrays of
// object-space vertex positions — constant topology across steps.
static void Scene_SetMeshVertexMotion(
    const SceneImplPtr & scene,
    const std::string &meshName,
    const py_float_array times,
    const std::vector<py_float_array> &vertsPerStep
) {
  auto t = times.unchecked<1>();
  const size_t timesCount = t.shape(0);
  if (vertsPerStep.size() != timesCount)
    throw std::runtime_error("Scene.SetMeshVertexMotion(): number of vertex "
        "steps differs from number of times");

  std::vector<float> flatVerts;
  size_t nVerts = 0;
  for (size_t s = 0; s < timesCount; ++s) {
    auto v = vertsPerStep[s].unchecked<2>();
    if (v.shape(1) != 3)
      throw std::runtime_error("Scene.SetMeshVertexMotion(): vertex step "
          + luxrays::ToString(s) + " must have shape [N,3]");
    if (s == 0)
      nVerts = v.shape(0);
    else if ((size_t)v.shape(0) != nVerts)
      throw std::runtime_error("Scene.SetMeshVertexMotion(): vertex steps "
          "have different vertex counts");

    const float *src = v.data(0, 0);
    flatVerts.insert(flatVerts.end(), src, src + (size_t)v.shape(0) * 3);
  }

  {
    py::gil_scoped_release release;
    scene->SetMeshVertexMotion(meshName,
        t.data(0), timesCount, flatVerts.data(), flatVerts.size());
  }
}

// Strand deformation motion (E9 phase 5b): `times` is a (S,) float
// array, `pointsPerStep` a list of S (P,3) float arrays of strand
// control-point positions — same strand layout/point counts as the
// DefineStrands input. Steps are re-tessellated inside the scene.
static void Scene_SetStrandsVertexMotion(
    const SceneImplPtr & scene,
    const std::string &meshName,
    const py_float_array times,
    const std::vector<py_float_array> &pointsPerStep
) {
  auto t = times.unchecked<1>();
  const size_t timesCount = t.shape(0);
  if (pointsPerStep.size() != timesCount)
    throw std::runtime_error("Scene.SetStrandsVertexMotion(): number of "
        "point steps differs from number of times");

  std::vector<float> flatPoints;
  size_t nPoints = 0;
  for (size_t s = 0; s < timesCount; ++s) {
    auto v = pointsPerStep[s].unchecked<2>();
    if (v.shape(1) != 3)
      throw std::runtime_error("Scene.SetStrandsVertexMotion(): point step "
          + luxrays::ToString(s) + " must have shape [P,3]");
    if (s == 0)
      nPoints = v.shape(0);
    else if ((size_t)v.shape(0) != nPoints)
      throw std::runtime_error("Scene.SetStrandsVertexMotion(): point steps "
          "have different control-point counts");

    const float *src = v.data(0, 0);
    flatPoints.insert(flatPoints.end(), src, src + (size_t)v.shape(0) * 3);
  }

  {
    py::gil_scoped_release release;
    scene->SetStrandsVertexMotion(meshName,
        t.data(0), timesCount, flatPoints.data(), flatPoints.size());
  }
}

static void Scene_DefineStrands(
    const SceneImplPtr & scene,
    const std::string &shapeName,
    const py::int_ strandsCount,
    const py::int_ pointsCount,
    const py::object &points,
    const py::object &segments,
    const py::object &thickness,
    const py::object &transparency,
    const py::object &colors,
    const py::object &uvs,
    const std::string &tessellationTypeStr,
    const py::int_ adaptiveMaxDepth,
    const float adaptiveError,
    const py::int_ solidSideCount,
    const bool solidCapBottom,
    const bool solidCapTop,
    const bool useCameraPosition) {
  // Initialize the cyHairFile object
  luxrays::cyHairFile strands;
  strands.SetHairCount(strandsCount);
  strands.SetPointCount(pointsCount);

  // Set defaults if available
  int flags = CY_HAIR_FILE_POINTS_BIT;

  if (py::isinstance<py::int_>(segments))
    strands.SetDefaultSegmentCount(py::cast<int>(segments));
  else
    flags |= CY_HAIR_FILE_SEGMENTS_BIT;

  if (py::isinstance<py::float_>(thickness))
    strands.SetDefaultThickness(py::cast<float>(thickness));
  else
    flags |= CY_HAIR_FILE_THICKNESS_BIT;

  if (py::isinstance<py::float_>(transparency))
    strands.SetDefaultTransparency(py::cast<float>(transparency));
  else
    flags |= CY_HAIR_FILE_TRANSPARENCY_BIT;

  if (py::isinstance<py::tuple>(colors)) {
    const py::tuple &t = py::cast<py::tuple>(colors);

    strands.SetDefaultColor(
      py::cast<float>(t[0]),
      py::cast<float>(t[1]),
      py::cast<float>(t[2]));
  } else
    flags |= CY_HAIR_FILE_COLORS_BIT;

  if (!uvs.is_none())
    flags |= CY_HAIR_FILE_UVS_BIT;

  strands.SetArrays(flags);

  // Translate all segments
  if (!py::isinstance<py::int_>(segments)) {
    if(py::isinstance<py::list>(segments)) {
      const py::list &l = py::cast<py::list>(segments);
      const py::ssize_t size = len(l);

      u_short *s = strands.GetSegmentsArray();
      for (py::ssize_t i = 0; i < size; ++i)
        s[i] = py::cast<u_short>(l[i]);
    } else {
      const std::string objType = py::cast<std::string>((segments.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of segments of method Scene.DefineStrands(): " + objType);
    }
  }


  // Translate all points
  if (!points.is_none()) {
    if(py::isinstance<py::list>(points)) {
      const py::list &l = py::cast<py::list>(points);
      const py::ssize_t size = len(l);

      float *p = strands.GetPointsArray();
      for (py::ssize_t i = 0; i < size; ++i) {
        if(py::isinstance<py::tuple>(l[i])) {
          const py::tuple &t = py::cast<py::tuple>(l[i]);
          p[i * 3] = py::cast<float>(t[0]);
          p[i * 3 + 1] = py::cast<float>(t[1]);
          p[i * 3 + 2] = py::cast<float>(t[2]);
        } else {
          const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
          throw std::runtime_error("Wrong data type in the list of points of method Scene.DefineStrands() at position " + luxrays::ToString(i) +": " + objType);
        }
      }
    } else {
      const std::string objType = py::cast<std::string>((points.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of points of method Scene.DefineStrands(): " + objType);
    }
  } else
    throw std::runtime_error("Points list can not be None in method Scene.DefineStrands()");

  // Translate all thickness
  if (!py::isinstance<py::float_>(thickness)) {
    if(py::isinstance<py::list>(thickness)) {
      const py::list &l = py::cast<py::list>(thickness);
      const py::ssize_t size = len(l);

      float *t = strands.GetThicknessArray();
      for (py::ssize_t i = 0; i < size; ++i)
        t[i] = py::cast<float>(l[i]);
    } else {
      const std::string objType = py::cast<std::string>((thickness.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of thickness of method Scene.DefineStrands(): " + objType);
    }
  }

  // Translate all transparency
  if (!py::isinstance<py::float_>(transparency)) {
    if(py::isinstance<py::list>(transparency)) {
      const py::list &l = py::cast<py::list>(transparency);
      const py::ssize_t size = len(l);

      float *t = strands.GetTransparencyArray();
      for (py::ssize_t i = 0; i < size; ++i)
        t[i] = py::cast<float>(l[i]);
    } else {
      const std::string objType = py::cast<std::string>((transparency.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of transparency of method Scene.DefineStrands(): " + objType);
    }
  }

  // Translate all colors
  if (!py::isinstance<py::tuple>(colors)) {
    if(py::isinstance<py::list>(colors)) {
      const py::list &l = py::cast<py::list>(colors);
      const py::ssize_t size = len(l);

      float *c = strands.GetColorsArray();
      for (py::ssize_t i = 0; i < size; ++i) {
        if(py::isinstance<py::tuple>(l[i])) {
          const py::tuple &t = py::cast<py::tuple>(l[i]);
          c[i * 3] = py::cast<float>(t[0]);
          c[i * 3 + 1] = py::cast<float>(t[1]);
          c[i * 3 + 2] = py::cast<float>(t[2]);
        } else {
          const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
          throw std::runtime_error("Wrong data type in the list of colors of method Scene.DefineStrands() at position " + luxrays::ToString(i) +": " + objType);
        }
      }
    } else {
      const std::string objType = py::cast<std::string>((colors.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of colors of method Scene.DefineStrands(): " + objType);
    }
  }

  // Translate all UVs
  if (!uvs.is_none()) {
    if(py::isinstance<py::list>(uvs)) {
      const py::list &l = py::cast<py::list>(uvs);
      const py::ssize_t size = len(l);

      float *uv = strands.GetUVsArray();
      for (py::ssize_t i = 0; i < size; ++i) {
        if(py::isinstance<py::tuple>(l[i])) {
          const py::tuple &t = py::cast<py::tuple>(l[i]);
          uv[i * 2] = py::cast<float>(t[0]);
          uv[i * 2 + 1] = py::cast<float>(t[1]);
        } else {
          const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));
          throw std::runtime_error("Wrong data type in the list of UVs of method Scene.DefineStrands() at position " + luxrays::ToString(i) +": " + objType);
        }
      }
    } else {
      const std::string objType = py::cast<std::string>((uvs.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Wrong data type for the list of UVs of method Scene.DefineStrands(): " + objType);
    }
  }

  Scene::StrandsTessellationType tessellationType;
  if (tessellationTypeStr == "ribbon")
    tessellationType = Scene::TESSEL_RIBBON;
  else if (tessellationTypeStr == "ribbonadaptive")
    tessellationType = Scene::TESSEL_RIBBON_ADAPTIVE;
  else if (tessellationTypeStr == "solid")
    tessellationType = Scene::TESSEL_SOLID;
  else if (tessellationTypeStr == "solidadaptive")
    tessellationType = Scene::TESSEL_SOLID_ADAPTIVE;
  else
    throw std::runtime_error("Tessellation type unknown in method Scene.DefineStrands(): " + tessellationTypeStr);

  {
    py::gil_scoped_release release;
    scene->DefineStrands(shapeName, strands,
        tessellationType, adaptiveMaxDepth, adaptiveError,
        solidSideCount, solidCapBottom, solidCapTop,
        useCameraPosition);
  }
}

static void Scene_DuplicateObject(const SceneImplPtr & scene,
    const std::string &srcObjName,
    const std::string &dstObjName,
    const py::object &transformation,
    const size_t objectID) {
  float mat[16];
  GetMatrix4x4(transformation, mat);

  {
    py::gil_scoped_release release;
    scene->DuplicateObject(srcObjName, dstObjName, mat, objectID);
  }
}

static void Scene_DuplicateObjectMulti(const SceneImplPtr & scene,
    const std::string &srcObjName,
    const std::string &dstObjNamePrefix,
    const unsigned int count,
    const py::object &transformations,
    const py::object &objectIDs) {
  if (!PyObject_CheckBuffer(objectIDs.ptr())){
    const std::string objType = py::cast<std::string>((objectIDs.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Unsupported data type in Scene.DuplicateObject() method: " + objType);
  }
  if (!PyObject_CheckBuffer(transformations.ptr())) {
    const std::string objType = py::cast<std::string>((transformations.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Unsupported data type in Scene.DuplicateObject() method: " + objType);
  }

  Py_buffer transformationsView;
  if (PyObject_GetBuffer(transformations.ptr(), &transformationsView, PyBUF_SIMPLE)) {
    const std::string objType = py::cast<std::string>((transformations.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Unable to get a data view in Scene.DuplicateObject() method: " + objType);
  }
  Py_buffer objectIDsView;
  if (PyObject_GetBuffer(objectIDs.ptr(), &objectIDsView, PyBUF_SIMPLE)) {
    PyBuffer_Release(&transformationsView);

    const std::string objType = py::cast<std::string>((transformations.attr("__class__")).attr("__name__"));
    throw std::runtime_error("Unable to get a data view in Scene.DuplicateObject() method: " + objType);
  }

  const size_t objectIDsBufferSize = sizeof(u_int) * count;
  if ((size_t)objectIDsView.len < objectIDsBufferSize) {
    const std::string errorMsg = "Not enough objectIDs in the buffer of Scene.DuplicateObject() method: " +
        luxrays::ToString(objectIDsView.len) + " instead of " + luxrays::ToString(objectIDsBufferSize);

    PyBuffer_Release(&objectIDsView);
    PyBuffer_Release(&transformationsView);

    throw std::runtime_error(errorMsg);
  }
  const size_t transformationsBufferSize = sizeof(float) * 16 * count;
  if ((size_t)transformationsView.len < transformationsBufferSize) {
    const std::string errorMsg = "Not enough matrices in the buffer of Scene.DuplicateObject() method: " +
        luxrays::ToString(transformationsView.len) + " instead of " + luxrays::ToString(transformationsBufferSize);

    PyBuffer_Release(&objectIDsView);
    PyBuffer_Release(&transformationsView);

    throw std::runtime_error(errorMsg);
  }

  float *transformationsBuffer = (float *)transformationsView.buf;
  u_int *objectIDsBuffer = (u_int *)objectIDsView.buf;

  {
    py::gil_scoped_release release;
    scene->DuplicateObject(srcObjName, dstObjNamePrefix, count,
        transformationsBuffer, objectIDsBuffer);
  }

  PyBuffer_Release(&transformationsView);
  PyBuffer_Release(&objectIDsView);
}

static void Scene_DuplicateMotionObject(const SceneImplPtr & scene,
    const std::string &srcObjName,
    const std::string &dstObjName,
    const size_t steps,
    const py::object &times,
    const py::object &transformations,
    const size_t objectID) {
  if (!times.is_none() && !transformations.is_none()) {
    if(py::isinstance<py::list>(times) && py::isinstance<py::list>(transformations)) {
      const py::list &timesList = py::cast<py::list>(times);
      const py::list &transformationsList = py::cast<py::list>(transformations);

      if ((len(timesList) != steps) || (len(transformationsList) != steps))
        throw std::runtime_error("Wrong number of elements for the times and/or the list of transformations of method Scene.DuplicateObject()");

      std::vector<float> timesVec(steps);
      std::vector<float> transVec(steps * 16);
      u_int transIndex = 0;

      for (u_int i = 0; i < steps; ++i) {
        timesVec[i] = py::cast<float>(timesList[i]);

        float mat[16];
        GetMatrix4x4(transformationsList[i], mat);
        for (u_int i = 0; i < 16; ++i)
          transVec[transIndex++] = mat[i];
      }

      {
        py::gil_scoped_release release;
        scene->DuplicateObject(srcObjName, dstObjName, steps, &timesVec[0], &transVec[0], objectID);
      }
    } else
      throw std::runtime_error("Wrong data type for the list of transformation values of method Scene.DuplicateObject()");
  } else
    throw std::runtime_error("None times and/or transformations in Scene.DuplicateObject(): " + srcObjName);
}

static void Scene_DuplicateMotionObjectMulti(const SceneImplPtr & scene,
    const std::string &srcObjName,
    const std::string &dstObjName,
    const unsigned int count,
    const size_t steps,
    const py::object &times,
    const py::object &transformations,
    const py::object &objectIDs) {

    if (!PyObject_CheckBuffer(times.ptr())){
      const std::string objType = py::cast<std::string>((times.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unsupported data type in Scene.DuplicateObject() method: " + objType);
    }
    if (!PyObject_CheckBuffer(transformations.ptr())){
      const std::string objType = py::cast<std::string>((transformations.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unsupported data type in Scene.DuplicateObject() method: " + objType);
    }
    if (!PyObject_CheckBuffer(objectIDs.ptr())){
      const std::string objType = py::cast<std::string>((objectIDs.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unsupported data type in Scene.DuplicateObject() method: " + objType);
    }

    Py_buffer timesView;
    if (PyObject_GetBuffer(times.ptr(), &timesView, PyBUF_SIMPLE)) {
      const std::string objType = py::cast<std::string>((times.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Scene.DuplicateObject() method: " + objType);
    }
    Py_buffer transformationsView;
    if (PyObject_GetBuffer(transformations.ptr(), &transformationsView, PyBUF_SIMPLE)) {
      PyBuffer_Release(&timesView);

      const std::string objType = py::cast<std::string>((transformations.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Scene.DuplicateObject() method: " + objType);
    }
    Py_buffer objectIDsView;
    if (PyObject_GetBuffer(objectIDs.ptr(), &objectIDsView, PyBUF_SIMPLE)) {
      PyBuffer_Release(&timesView);
      PyBuffer_Release(&transformationsView);

      const std::string objType = py::cast<std::string>((objectIDs.attr("__class__")).attr("__name__"));
      throw std::runtime_error("Unable to get a data view in Scene.DuplicateObject() method: " + objType);
    }

    const size_t timesBufferSize = sizeof(float) * count;
    if ((size_t)timesView.len < timesBufferSize) {
      const std::string errorMsg = "Not enough times in the buffer of Scene.DuplicateObject() method: " +
          luxrays::ToString(timesView.len) + " instead of " + luxrays::ToString(timesBufferSize);

      PyBuffer_Release(&timesView);
      PyBuffer_Release(&transformationsView);
      PyBuffer_Release(&objectIDsView);

      throw std::runtime_error(errorMsg);
    }
    const size_t transformationsBufferSize = sizeof(float) * 16 * count;
    if ((size_t)transformationsView.len < transformationsBufferSize) {
      const std::string errorMsg = "Not enough matrices in the buffer of Scene.DuplicateObject() method: " +
          luxrays::ToString(transformationsView.len) + " instead of " + luxrays::ToString(transformationsBufferSize);

      PyBuffer_Release(&timesView);
      PyBuffer_Release(&transformationsView);
      PyBuffer_Release(&objectIDsView);

      throw std::runtime_error(errorMsg);
    }
    const size_t objectIDsBufferSize = sizeof(u_int) * count;
    if ((size_t)objectIDsView.len < objectIDsBufferSize) {
      const std::string errorMsg = "Not enough object IDs in the buffer of Scene.DuplicateObject() method: " +
          luxrays::ToString(objectIDsView.len) + " instead of " + luxrays::ToString(objectIDsBufferSize);

      PyBuffer_Release(&timesView);
      PyBuffer_Release(&transformationsView);
      PyBuffer_Release(&objectIDsView);

      throw std::runtime_error(errorMsg);
    }

    float *timesBuffer = (float *)timesView.buf;
    float *transformationsBuffer = (float *)transformationsView.buf;
    u_int *objectIDsBuffer = (u_int *)objectIDsView.buf;
    {
      py::gil_scoped_release release;
      scene->DuplicateObject(srcObjName, dstObjName, count, steps, timesBuffer,
          transformationsBuffer, objectIDsBuffer);
    }

    PyBuffer_Release(&timesView);
    PyBuffer_Release(&transformationsView);
    PyBuffer_Release(&objectIDsView);
}

static void Scene_DeleteObjects(const SceneImplPtr & scene,
    const py::list &l) {
  const py::ssize_t size = len(l);
  std::vector<std::string> names;
  names.reserve(size);
  for (py::ssize_t i = 0; i < size; ++i) {
    const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));

    if (objType == "str")
      names.push_back(py::cast<std::string>(l[i]));
    else
      throw std::runtime_error("Unsupported data type included in Scene.DeleteObjects() list: " + objType);
  }

  {
    py::gil_scoped_release release;
    scene->DeleteObjects(names);
  }
}

static void Scene_DeleteLights(const SceneImplPtr & scene,
    const py::list &l) {
  const py::ssize_t size = len(l);
  std::vector<std::string> names;
  names.reserve(size);
  for (py::ssize_t i = 0; i < size; ++i) {
    const std::string objType = py::cast<std::string>((l[i].attr("__class__")).attr("__name__"));

    if (objType == "str")
      names.push_back(py::cast<std::string>(l[i]));
    else
      throw std::runtime_error("Unsupported data type included in Scene.DeleteLights() list: " + objType);
  }

  {
    py::gil_scoped_release release;
    scene->DeleteLights(names);
  }
}

static void Scene_UpdateObjectTransformation(const SceneImplPtr & scene,
    const std::string &objName,
    const py::object &transformation
) {
	float mat[16];
	GetMatrix4x4(transformation, mat);
	{
		py::gil_scoped_release release;
		scene->UpdateObjectTransformation(objName, mat);
	}
}

//------------------------------------------------------------------------------
// Glue for RenderConfig class
//------------------------------------------------------------------------------

static py::tuple RenderConfig_LoadResumeFile(const py::str &fileNameStr) {
  const std::string fileName = py::cast<std::string>(fileNameStr);
  RenderStateImplRPtr startState;
  FilmImplUPtr startFilm;
  auto config = RenderConfigImpl::Create<
	  const std::string&, RenderStateImplRPtr&, FilmImplUPtr&
  >(fileName, startState, startFilm);

  // Cast each element explicitly: make_tuple stores the unique_ptrs and
  // the tuple caster forwards them as const&, which pybind rejects
  // ("Invalid return_value_policy for const unique_ptr&").
  py::tuple result(3);
  result[0] = py::cast(std::move(config));
  result[1] = py::cast(startState);
  result[2] = py::cast(std::move(startFilm));
  return result;
}

static std::unique_ptr<luxcore::detail::RenderConfigImpl>
RenderConfig_LoadFile(const py::str &fileNameStr) {
  const std::string fileName = py::cast<std::string>(fileNameStr);
  auto config = RenderConfigImpl::Create(fileName);

  return config;
}

static const luxcore::detail::SceneImpl&
RenderConfig_GetScene(luxcore::detail::RenderConfigImpl *renderConfig) {
	auto& luxscene = renderConfig->GetScene();
  return static_cast<const luxcore::detail::SceneImpl&>(luxscene);
}

static py::tuple RenderConfig_GetFilmSize(luxcore::detail::RenderConfigImpl *renderConfig) {
  u_int filmWidth, filmHeight, filmSubRegion[4];
  const bool result = renderConfig->GetFilmSize(&filmWidth, &filmHeight, filmSubRegion);

  return py::make_tuple(filmWidth, filmHeight,
      py::make_tuple(filmSubRegion[0], filmSubRegion[1], filmSubRegion[2], filmSubRegion[3]),
      result);
}

//------------------------------------------------------------------------------
// Glue for RenderSession class
//------------------------------------------------------------------------------


class DenoisingImagePipeline {
    std::mutex mutex;
    std::unique_ptr<slg::ImagePipeline> pipeline;
    slg::FilmUPtr scaledFilm;
    uint64_t epoch = 0, revision = 0, lastFrame = 0;
    unsigned width = 0, height = 0;
    unsigned outputWidth = 0, outputHeight = 0;
public:
    DenoisingImagePipeline() = default;
    py::array_t<float> Process(const py::dict &packet, const py::object &rgba, const bool reset) {
        if (!packet.contains("_frame_lease"))
            throw std::runtime_error("A native coherent frame lease is required for image-pipeline processing");
        const auto capsule = packet["_frame_lease"].cast<py::capsule>();
        if (!capsule.name() || std::strcmp(capsule.name(), "superluxcore.denoising_frame"))
            throw std::runtime_error("Invalid native frame lease");
        const auto snapshot = *capsule.get_pointer<std::shared_ptr<const slg::DenoisingFrameSnapshot>>();
        if (!snapshot || !snapshot->presentationFilm)
            throw std::runtime_error(snapshot && !snapshot->presentationError.empty() ?
                snapshot->presentationError : "Coherent frame image-pipeline capture is disabled");
        py::ssize_t h = snapshot->presentationFilm->GetHeight(), w = snapshot->presentationFilm->GetWidth();
        py::array input;
        const float *pixels = nullptr;
        if (!rgba.is_none()) {
            if (!py::isinstance<py::array>(rgba))
                throw std::runtime_error("Image-pipeline input must be a contiguous float32 RGBA array");
            input = rgba.cast<py::array>();
            if (!input.dtype().is(py::dtype::of<float>()) || !(input.flags() & py::array::c_style) ||
                input.ndim() != 3 || input.shape(0) <= 0 || input.shape(0) > 16384 ||
                input.shape(1) <= 0 || input.shape(1) > 16384 || input.shape(2) != 4)
                throw std::runtime_error("Image-pipeline input must have supported dimensions and contiguous float32 RGBA layout");
            h = input.shape(0); w = input.shape(1);
            pixels = static_cast<const float *>(input.data());
        }
        if (snapshot->inputReduction > 1u &&
                (w != snapshot->targetWidth || h != snapshot->targetHeight || snapshot->targetAlpha.empty()))
            throw std::runtime_error("Canonical coverage reconstruction requires the original target raster and coverage");
        const bool scaled = w != snapshot->presentationFilm->GetWidth() || h != snapshot->presentationFilm->GetHeight();
        if (scaled && slg::ScaledPresentationBytes(*snapshot->presentationFilm, w, h) > (size_t(1) << 30))
            throw std::runtime_error("Reconstructed presentation context and output exceed the 1 GiB budget");
        py::array_t<float> result({h, w, py::ssize_t(4)});
        float *output = result.mutable_data();
        {
            py::gil_scoped_release release;
            std::lock_guard<std::mutex> ownLock(mutex);
            std::lock_guard<std::mutex> frameLock(snapshot->presentationMutex);
            auto &source = *snapshot->presentationFilm;
            if (source.GetImagePipelineCount() == 0)
                throw std::runtime_error("The captured frame has no image pipeline");
            const bool fresh = reset || !pipeline || epoch != snapshot->sceneEpoch ||
                revision != snapshot->presentationRevision || width != snapshot->width || height != snapshot->height ||
                outputWidth != w || outputHeight != h;
            if (!fresh && snapshot->frameId <= lastFrame)
                throw std::runtime_error("Image-pipeline frames must arrive in increasing order");
            if (pixels)
                for (size_t i = 0; i < size_t(w) * h * 4; ++i)
                    if (!std::isfinite(pixels[i])) throw std::runtime_error("Nonfinite image-pipeline input");
            if (fresh) {
                pipeline.reset(source.GetImagePipeline(0)->Copy());
                scaledFilm.reset();
            }
            if (scaled) {
                if (!scaledFilm) scaledFilm = slg::CreateScaledPresentation(source, w, h);
                slg::CopyScaledPresentationChannels(source, *scaledFilm);
            }
            auto &film = scaled ? *scaledFilm : source;
            // Preserve original AOV alpha for later raw fallback/reference reads.
            struct AlphaRestore {
                slg::Film &film;
                std::vector<float> saved;
                AlphaRestore(slg::Film &f, bool needed): film(f) {
                    if (needed) {
                        const float *a = film.channel_ALPHA->GetPixels();
                        saved.assign(a, a + size_t(film.GetPixelCount()) * 2);
                    }
                }
                ~AlphaRestore() { if (!saved.empty()) std::copy(saved.begin(), saved.end(), film.channel_ALPHA->GetPixels()); }
            } alpha(film, pixels != nullptr && !scaled);
            try {
                if (pixels) {
                    float *target = film.channel_IMAGEPIPELINEs[0]->GetPixels();
                    float *a = film.channel_ALPHA->GetPixels();
                    for (size_t i = 0; i < size_t(w) * h; ++i) {
                        std::copy(pixels + i * 4, pixels + i * 4 + 3, target + i * 3);
                        if (!snapshot->targetHoldoutRGB.empty() && snapshot->targetHoldout[i])
                            std::copy_n(snapshot->targetHoldoutRGB.data() + i * 3, 3, target + i * 3);
                        a[i * 2] = snapshot->targetAlpha.empty() ? pixels[i * 4 + 3] : snapshot->targetAlpha[i];
                        a[i * 2 + 1] = 1.f;
                    }
                } else {
                    film.MergeImagePipelineInput(0);
                }
                // HDR already contains LightMix. Apply plugins directly;
                // ExecuteImagePipeline would merge/scale the input again.
                pipeline->Apply(film, 0);
                film.GetOutput<float>(slg::FilmOutputs::RGBA_IMAGEPIPELINE, output, 0, false);
                for (size_t i = 0; i < size_t(w) * h * 4; ++i)
                    if (!std::isfinite(output[i])) throw std::runtime_error("Nonfinite image-pipeline output");
            } catch (...) { pipeline.reset(); scaledFilm.reset(); throw; }
            epoch = snapshot->sceneEpoch; revision = snapshot->presentationRevision;
            width = snapshot->width; height = snapshot->height; lastFrame = snapshot->frameId;
            outputWidth = w; outputHeight = h;
        }
        return result;
    }
};

static void RenderSession_SetDenoisingFrameHaltSPP(RenderSessionImpl &session, const uint64_t samples) {
#if !defined(LUXRAYS_DISABLE_OPENCL)
    if (!session.IsStarted() || !dynamic_cast<slg::RTPathOCLRenderEngine *>(session.GetSLGRenderSession().renderEngine.get()))
        throw std::runtime_error("Updating independent-frame halt requires an active RTPATHOCL session");
    auto props = std::make_unique<luxrays::Properties>();
    props->Set(luxrays::Property("rtpath.denoisingframes.haltspp")(static_cast<u_longlong>(samples)));
    session.Parse(props);
#else
    throw std::runtime_error("Updating independent-frame halt requires GPU support");
#endif
}

static py::object RenderSession_GetDenoisingFrame(RenderSessionImpl &session,
        const uint64_t afterId, const u_int timeoutMs) {
#if !defined(LUXRAYS_DISABLE_OPENCL)
    std::shared_ptr<const slg::DenoisingFrameSnapshot> snapshot;
    {
        py::gil_scoped_release release;
        if (session.IsStarted()) {
            auto *engine = dynamic_cast<slg::RTPathOCLRenderEngine *>(session.GetSLGRenderSession().renderEngine.get());
            if (!engine) throw std::runtime_error("Independent frame streaming requires RTPATHOCL");
            snapshot = engine->GetDenoisingFrame(afterId, timeoutMs);
        }
    }
    if (!snapshot) return py::none();
    const py::ssize_t h=snapshot->height,w=snapshot->width;
    // The capsule owns a read-only lease. No film lock/GPU readback or extra
    // full-frame copy occurs on this consumer call; newer frames can replace
    // the publisher's slot while these arrays are being consumed.
    auto *lease = new std::shared_ptr<const slg::DenoisingFrameSnapshot>(snapshot);
    py::capsule owner(lease, "superluxcore.denoising_frame", [](void *p) { delete static_cast<std::shared_ptr<const slg::DenoisingFrameSnapshot> *>(p); });
    py::array_t<float> color({h,w,py::ssize_t(4)},snapshot->color.data(),owner);
    py::array_t<float> guides({h,w,py::ssize_t(slg::DENOISING_GUIDE_STRIDE)},snapshot->guides.data(),owner);
    py::array_t<u_int> counts({h,w},snapshot->sampleCount.data(),owner);
    py::array_t<float> ctw({py::ssize_t(4),py::ssize_t(4)},snapshot->cameraToWorld,owner);
    py::array_t<float> wtc({py::ssize_t(4),py::ssize_t(4)},snapshot->worldToCamera,owner);
    py::array_t<float> ctr({py::ssize_t(4),py::ssize_t(4)},snapshot->cameraToRaster,owner);
    for (const auto &a : {py::object(color),py::object(guides),py::object(counts),py::object(ctw),py::object(wtc),py::object(ctr)})
        a.attr("setflags")(false);
    py::dict result;
    result["color"]=color;result["guides"]=guides;result["sample_count"]=counts;
    result["camera_to_world"]=ctw;result["world_to_camera"]=wtc;result["camera_to_raster"]=ctr;
    result["frame_id"]=snapshot->frameId;result["scene_epoch"]=snapshot->sceneEpoch;
    result["jitter"]=py::make_tuple(snapshot->jitterX,snapshot->jitterY);
    result["complete"]=snapshot->complete;result["completed_time"]=snapshot->completedTime;
    result["motion_camera_compatible"]=snapshot->motionCameraCompatible;
    result["source"]="renderer_gpu";result["independent_samples"]=true;result["version"]=1;
    result["guide_version"]=2;
    result["target_size"]=py::make_tuple(snapshot->targetWidth, snapshot->targetHeight);
    result["input_reduction"]=snapshot->inputReduction;
    if (!snapshot->targetAlpha.empty()) {
        const py::ssize_t th = snapshot->targetHeight, tw = snapshot->targetWidth;
        py::array_t<float> alpha({th,tw}, snapshot->targetAlpha.data(), owner);
        py::array_t<uint8_t> holdout({th,tw}, snapshot->targetHoldout.data(), owner);
        alpha.attr("setflags")(false); holdout.attr("setflags")(false);
        result["target_alpha"] = alpha; result["target_holdout"] = holdout;
        if (!snapshot->targetHoldoutRGB.empty()) {
            py::array_t<float> rgb({th,tw,py::ssize_t(3)}, snapshot->targetHoldoutRGB.data(), owner);
            rgb.attr("setflags")(false); result["target_holdout_rgb"] = rgb;
        }
        result["coverage_source"] = "renderer_full_resolution";
    }
    result["_frame_lease"]=owner;
    result["image_pipeline_available"]=bool(snapshot->presentationFilm);
    result["image_pipeline_revision"]=snapshot->presentationRevision;
    result["image_pipeline_channel_bytes"]=snapshot->presentationChannelBytes;
    result["image_pipeline_error"]=snapshot->presentationError;
    result["lossless_delivery"]=snapshot->losslessDelivery;
    result["image_pipeline_accumulated"]=snapshot->presentationAccumulated;
    result["accumulated_eye_samples"]=snapshot->accumulatedEyeSamples;
    return std::move(result);
#else
    throw std::runtime_error("This build has no GPU frame producer");
#endif
}

static void RenderSession_AcknowledgeDenoisingFrame(RenderSessionImpl &session, const py::dict &packet, const bool keepLossless) {
#if !defined(LUXRAYS_DISABLE_OPENCL)
    if (!packet.contains("_frame_lease"))
        throw std::runtime_error("A native coherent frame lease is required for acknowledgement");
    const auto capsule = packet["_frame_lease"].cast<py::capsule>();
    if (!capsule.name() || std::strcmp(capsule.name(), "superluxcore.denoising_frame"))
        throw std::runtime_error("Invalid native frame lease");
    const auto snapshot = *capsule.get_pointer<std::shared_ptr<const slg::DenoisingFrameSnapshot>>();
    py::gil_scoped_release release;
    if (!session.IsStarted()) throw std::runtime_error("The render session is not started");
    auto *engine = dynamic_cast<slg::RTPathOCLRenderEngine *>(session.GetSLGRenderSession().renderEngine.get());
    if (!engine) throw std::runtime_error("Lossless frame delivery requires RTPATHOCL");
    engine->AcknowledgeDenoisingFrame(snapshot, keepLossless);
#else
    throw std::runtime_error("This build has no GPU frame producer");
#endif
}

static void RenderSession_ReleaseDenoisingFrameDelivery(RenderSessionImpl &session) {
#if !defined(LUXRAYS_DISABLE_OPENCL)
    py::gil_scoped_release release;
    if (!session.IsStarted()) throw std::runtime_error("The render session is not started");
    auto *engine = dynamic_cast<slg::RTPathOCLRenderEngine *>(session.GetSLGRenderSession().renderEngine.get());
    if (!engine) throw std::runtime_error("Independent frame delivery requires RTPATHOCL");
    engine->ReleaseDenoisingFrameDelivery();
#else
    throw std::runtime_error("This build has no GPU frame producer");
#endif
}

static py::dict RenderSession_GetDenoisingGuides(RenderSessionImpl &session) {
#if !defined(LUXRAYS_DISABLE_OPENCL)
    std::vector<float> records;
    size_t w, h;
    {
        py::gil_scoped_release release;
        if (!session.IsStarted() || session.IsInSceneEdit() || (!session.IsInPause() && !session.HasDone()))
            throw std::runtime_error("Pause or finish the frame before reading renderer-owned guides");
        auto &native = session.GetSLGRenderSession();
        const auto *engine = dynamic_cast<const slg::PathOCLBaseRenderEngine *>(native.renderEngine.get());
        if (!engine) throw std::runtime_error("Renderer-owned GPU guides are unavailable on this engine");
        records = engine->GetDenoisingGuideSnapshot();
        w = native.film->GetWidth(); h = native.film->GetHeight();
        if (records.size() != w*h*slg::DENOISING_GUIDE_STRIDE)
            throw std::runtime_error("Renderer-owned guide dimensions changed during the read");
    }
    py::array_t<float> diffuse({h,w,size_t(4)}), specular({h,w,size_t(4)});
    py::array_t<float> normal({h,w,size_t(4)}), position({h,w,size_t(4)});
    py::array_t<uint8_t> status({h,w});
    float *d=diffuse.mutable_data(), *s=specular.mutable_data();
    float *n=normal.mutable_data(), *p=position.mutable_data();
    uint8_t *flags=status.mutable_data();
    {
        py::gil_scoped_release release;
        for (size_t i=0;i<w*h;++i) {
            const float *r=records.data()+i*slg::DENOISING_GUIDE_STRIDE;
            for (size_t k=0;k<3;++k) {d[4*i+k]=r[k];s[4*i+k]=r[4+k];n[4*i+k]=r[8+k];p[4*i+k]=r[12+k];}
            d[4*i+3]=1.f;s[4*i+3]=0.f;n[4*i+3]=r[3];p[4*i+3]=r[15];
            flags[i]=r[7]==4.f?0:(r[7]==0.f?2:static_cast<uint8_t>(r[7]));
        }
    }
    py::dict result;
    result["diffuse"]=diffuse;result["specular"]=specular;
    result["normal_roughness"]=normal;result["position_depth"]=position;result["status"]=status;
    result["source"]="renderer_gpu";result["version"]=1;
    result["contract"]="Actual GPU primary hit/sample; 0 background, 1 supported, 2 unsupported/unwritten, 3 continuation required";
    return result;
#else
    throw std::runtime_error("This build has no GPU guide producer");
#endif
}

static py::dict RenderSession_GetDenoisingSurfaceGuides(RenderSessionImpl &session,
        const float jitterX, const float jitterY) {
    // Host reference producer. The caller serializes this with session edits;
    // it never mutates a film, camera, sampler, transport setting or GPU task.
    if (!std::isfinite(jitterX) || !std::isfinite(jitterY) ||
            fabsf(jitterX) > .5f || fabsf(jitterY) > .5f)
        throw std::runtime_error("Denoising guides require finite half-pixel jitter");
    if (!session.IsStarted() || session.IsInSceneEdit())
        throw std::runtime_error("Denoising guides require a preprocessed session outside scene edits");
    if (!session.IsInPause() && !session.HasDone())
        throw std::runtime_error("Pause or finish the frame before reading denoising guides");
    auto &native = session.GetSLGRenderSession();
    const auto &scene = native.renderConfig.GetScene();
    const auto &camera = scene.GetCamera();
    if (camera.GetType() != slg::Camera::PERSPECTIVE || camera.motionSystem ||
            static_cast<const slg::PerspectiveCamera &>(camera).lensRadius != 0.f)
        throw std::runtime_error("Surface guide reference currently supports static pinhole perspective cameras");
    const size_t w = native.film->GetWidth(), h = native.film->GetHeight();
    // Four float4 planes plus status: bounded independently of Python inputs.
    if (!w || !h || w > 16384 || h > 16384 || w * h > (256u << 20) / 65)
        throw std::runtime_error("Surface guides exceed the 256 MiB output budget");
    py::array_t<float> diffuse({h, w, size_t(4)}), specular({h, w, size_t(4)});
    py::array_t<float> normal({h, w, size_t(4)}), position({h, w, size_t(4)});
    py::array_t<uint8_t> status({h, w});
    float *d = diffuse.mutable_data(), *s = specular.mutable_data();
    float *n = normal.mutable_data(), *p = position.mutable_data();
    uint8_t *flags = status.mutable_data();
    {
        py::gil_scoped_release release;
        const auto &worldToCamera = camera.GetWorldToCamera();
        const float time = camera.GenerateRayTime(.5f);
        for (size_t y = 0; y < h; ++y) for (size_t x = 0; x < w; ++x) {
            const size_t i = y * w + x, j = 4 * i;
            for (size_t k = 0; k < 4; ++k) d[j+k] = s[j+k] = n[j+k] = p[j+k] = 0.f;
            d[j+3] = n[j+3] = 1.f;
            p[j+3] = camera.clipYon;
            flags[i] = 0;
            slg::PathVolumeInfo volume;
            luxrays::Ray ray;
            camera.GenerateRay(time, x + .5f + jitterX, y + .5f + jitterY,
                    &ray, &volume, .5f, .5f);
            luxrays::RayHit hit;
            slg::BSDF bsdf;
            luxrays::Spectrum connection;
            if (!scene.Intersect(nullptr, slg::EYE_RAY | slg::CAMERA_RAY,
                    &volume, .5f, &ray, &hit, &bsdf, &connection)) continue;
            const auto &hp = bsdf.hitPoint;
            p[j] = hp.p.x; p[j+1] = hp.p.y; p[j+2] = hp.p.z;
            p[j+3] = (worldToCamera * hp.p).z;
            slg::DenoisingFeatures features;
            flags[i] = 2;
            if (bsdf.IsVolume() || !bsdf.GetMaterial()->GetDenoisingFeatures(hp, features)) continue;
            for (size_t k = 0; k < 3; ++k) {
                d[j+k] = features.diffuse.c[k];
                s[j+k] = features.specular.c[k];
            }
            if (features.weight > 0.f) {
                const luxrays::Vector cn = worldToCamera * (features.normal / features.weight);
                n[j] = cn.x; n[j+1] = cn.y; n[j+2] = cn.z;
                n[j+3] = features.roughness / features.weight;
            }
            const bool transparent = connection.c[0] != 1.f || connection.c[1] != 1.f || connection.c[2] != 1.f;
            flags[i] = (features.transmission || features.FeatureWeight() < 1.f || transparent) ? 3 : 1;
        }
    }
    py::dict result;
    result["diffuse"] = diffuse; result["specular"] = specular;
    result["normal_roughness"] = normal; result["position_depth"] = position;
    result["status"] = status;
    result["contract"] = "host primary-surface estimates; 0 background, 1 supported, 2 unsupported, 3 requires path continuation";
    return result;
}

static std::shared_ptr<luxcore::detail::RenderStateImpl>
RenderSession_GetRenderState(
	const std::unique_ptr<luxcore::detail::RenderSessionImpl> & renderSession
) {
  return static_pointer_cast<luxcore::detail::RenderStateImpl>(renderSession->GetRenderState());
}

//------------------------------------------------------------------------------
// Film output conversion functions
//------------------------------------------------------------------------------

static float FindMaxValue(const float *buffer, const size_t buffersize) {
	float maxValue = 0;
	for (u_int i = 0; i < buffersize; ++i) {
		const float value = buffer[i];
		if (!isinf(value) && !isnan(value) && (value > maxValue)) {
			maxValue = value;
		}
	}
	return maxValue;
}

// Note: This function is used by pysuperluxcoredemo.py, do not remove.
// Note2: This function could be usefully implemented in Python/numpy (TODO).
void ConvertFilmChannelOutput_3xFloat_To_4xUChar(const size_t width, const size_t height,
		py::object &objSrc, py::object &objDst, const bool normalize) {
	if (!PyObject_CheckBuffer(objSrc.ptr())) {
		const std::string objType = py::cast<std::string>((objSrc.attr("__class__")).attr("__name__"));
		throw std::runtime_error("Unsupported data type in source object of ConvertFilmChannelOutput_3xFloat_To_4xUChar(): " + objType);
	}
	if (!PyObject_CheckBuffer(objDst.ptr())) {
		const std::string objType = py::cast<std::string>((objDst.attr("__class__")).attr("__name__"));
		throw std::runtime_error("Unsupported data type in destination object of ConvertFilmChannelOutput_3xFloat_To_4xUChar(): " + objType);
	}

	Py_buffer srcView;
	if (PyObject_GetBuffer(objSrc.ptr(), &srcView, PyBUF_SIMPLE)) {
		const std::string objType = py::cast<std::string>((objSrc.attr("__class__")).attr("__name__"));
		throw std::runtime_error("Unable to get a source data view in ConvertFilmChannelOutput_3xFloat_To_4xUChar(): " + objType);
	}
	Py_buffer dstView;
	if (PyObject_GetBuffer(objDst.ptr(), &dstView, PyBUF_SIMPLE)) {
		PyBuffer_Release(&srcView);

		const std::string objType = py::cast<std::string>((objSrc.attr("__class__")).attr("__name__"));
		throw std::runtime_error("Unable to get a source data view in ConvertFilmChannelOutput_3xFloat_To_4xUChar(): " + objType);
	}

	if (srcView.len / (3 * 4) != dstView.len / 4) {
		PyBuffer_Release(&srcView);
		PyBuffer_Release(&dstView);
		throw std::runtime_error("Wrong buffer size in ConvertFilmChannelOutput_3xFloat_To_4xUChar()");
	}

	const float *src = (float *)srcView.buf;
	u_char *dst = (u_char *)dstView.buf;

	if (normalize) {
		const float maxValue = FindMaxValue(src, width * height * 3);
		const float k = (maxValue == 0.f) ? 0.f : (255.f / maxValue);

		for (u_int y = 0; y < height; ++y) {
			u_int srcIndex = (height - y - 1) * width * 3;
			u_int dstIndex = y * width * 4;

			for (u_int x = 0; x < width; ++x) {
				dst[dstIndex++] = (u_char)floor((src[srcIndex + 2] * k + .5f));
				dst[dstIndex++] = (u_char)floor((src[srcIndex + 1] * k + .5f));
				dst[dstIndex++] = (u_char)floor((src[srcIndex] * k + .5f));
				dst[dstIndex++] = 0xff;
				srcIndex += 3;
			}
		}
	} else {
		for (u_int y = 0; y < height; ++y) {
			u_int srcIndex = (height - y - 1) * width * 3;
			u_int dstIndex = y * width * 4;

			for (u_int x = 0; x < width; ++x) {
				dst[dstIndex++] = (u_char)floor((src[srcIndex + 2] * 255.f + .5f));
				dst[dstIndex++] = (u_char)floor((src[srcIndex + 1] * 255.f + .5f));
				dst[dstIndex++] = (u_char)floor((src[srcIndex] * 255.f + .5f));
				dst[dstIndex++] = 0xff;
				srcIndex += 3;
			}
		}
	}

	PyBuffer_Release(&srcView);
	PyBuffer_Release(&dstView);
}

//------------------------------------------------------------------------------

PYBIND11_MODULE(pysuperluxcore, m) {
#if !defined(LUXRAYS_DISABLE_OPENCL)
  m.attr("RT_FILM_RESOLUTION_POLICY_VERSION") = 1;
#else
  m.attr("RT_FILM_RESOLUTION_POLICY_VERSION") = 0;
#endif
  // I get a crash on Ubuntu 19.10 without this line and this should be
  // good anyway to avoid problems with "," Vs. "." decimal separator, etc.

  try {
    std::locale::global(std::locale("C.UTF-8"));
  } catch (std::runtime_error &) {
    // "C.UTF-8" std::locale may not exist on some system so I ignore the error
  }

  // This 'module' is actually a fake package
  m.attr("__path__") = "pysuperluxcore";
  m.attr("__package__") = "pysuperluxcore";
  m.attr("__doc__") = "LuxCoreRender Python bindings\n\n"
      "Provides access to the LuxCoreRender API in python\n\n";
  m.attr("__version__") = LUXCORE_VERSION;

  m.def("Version", LuxCoreVersion, "Returns the LuxCore version");

  m.def("Init", &LuxCore_Init);
  py::class_<DenoisingImagePipeline>(m, "DenoisingImagePipeline")
      .def(py::init<>())
      .def_static("supports_upscaling", []() { return true; })
      .def_static("supports_canonical_coverage", []() { return true; })
      .def("process", &DenoisingImagePipeline::Process,
          py::arg("frame"), py::arg("rgba") = py::none(), py::arg("reset") = false);
  m.def("Init", &LuxCore_InitDefaultHandler);
  m.def("SetLogHandler", &LuxCore_SetLogHandler);

  // A handler still registered at interpreter exit crashed every
  // standalone script that called Init(handler) (SIGSEGV after the
  // last line, 2.11.32 wheel too): the static py::object was released
  // after finalization, and a late log line could call into the dead
  // interpreter. Drop it while Python is still alive.
  py::module_::import("atexit").attr("register")(py::cpp_function([]() {
    SetLogHandler(NULL);
    luxCoreLogHandler = py::object();
  }));
  m.def("ParseLXS", &ParseLXS);
  m.def("MakeTx", &MakeTx);

  m.def("GetPlatformDesc", &GetPlatformDesc);
  m.def("GetOpenCLDeviceDescs", &GetOpenCLDeviceDescs);

  // Deprecated, use GetOpenCLDeviceDescs instead
  m.def("GetOpenCLDeviceList", &GetOpenCLDeviceList);
  m.def("GetDeviceList", &GetDeviceList);

  m.def("ClearFileNameResolverPaths", &ClearFileNameResolverPaths);
  m.def("AddFileNameResolverPath", &AddFileNameResolverPath);
  m.def("GetFileNameResolverPaths", &GetFileNameResolverPaths);

  // Long running call (compiles all GPU kernels): release the GIL so
  // Blender's UI thread stays alive, and offer an optional
  // KernelCacheFill(config, callable(index, count)) progress callback.
  m.def("KernelCacheFill", &LuxCore_KernelCacheFill1,
      py::call_guard<py::gil_scoped_release>());
  m.def("KernelCacheFill", &LuxCore_KernelCacheFill2,
      py::call_guard<py::gil_scoped_release>());
  m.def("KernelCacheFill", &LuxCore_KernelCacheFill3,
      py::call_guard<py::gil_scoped_release>());

  //--------------------------------------------------------------------------
  // Property class
  //--------------------------------------------------------------------------

  py::class_<luxrays::Property, py::smart_holder>(m, "Property")
    .def(
		py::init(
			[](std::string name) -> std::unique_ptr<luxrays::Property>
			{ return std::make_unique<luxrays::Property>(name); }
		)
	)
    .def(
		py::init(
			[](std::string name, bool val) -> std::unique_ptr<luxrays::Property>
			{ return std::make_unique<luxrays::Property>(name, val); }
		)
	)
    .def(
		py::init(
			[](std::string name, long long val) -> std::unique_ptr<luxrays::Property>
			{ return std::make_unique<luxrays::Property>(name, val); }
		)
	)
    .def(
		py::init(
			[](std::string name, double val) -> std::unique_ptr<luxrays::Property>
			{ return std::make_unique<luxrays::Property>(name, val); }
		)
	)
    .def(
		py::init(
			[](std::string name, std::string val) -> std::unique_ptr<luxrays::Property>
			{ return std::make_unique<luxrays::Property>(name, val); }
		)
	)

    //.def(py::init<std::string, bool>())
    //.def(py::init<std::string, long long>())
    //.def(py::init<std::string, double>())
    //.def(py::init<std::string, std::string>())
    .def(py::init(&Property_InitWithList))

    .def("GetName", &luxrays::Property::GetName, py::return_value_policy::copy)
    .def("GetSize", &luxrays::Property::GetSize)
    .def("Clear", &luxrays::Property::Clear)

    .def("Get", &Property_Get)

    .def<bool (luxrays::Property::*)(const u_int) const>
      ("GetBool", &luxrays::Property::Get)
    .def<long long (luxrays::Property::*)(const u_int) const>
      ("GetInt", &luxrays::Property::Get)
    .def<double (luxrays::Property::*)(const u_int) const>
      ("GetFloat", &luxrays::Property::Get)
    .def<std::string (luxrays::Property::*)(const u_int) const>
      ("GetString", &luxrays::Property::Get)
    .def("GetBlob", &Property_GetBlobByIndex)

    .def("GetBool", &Property_GetBool)
    .def("GetInt", &Property_GetInt)
    .def("GetUnsignedLongLong", &Property_GetUnsignedLongLong)
    .def("GetFloat", &Property_GetFloat)
    .def("GetString", &Property_GetString)
    .def("GetBlob", &Property_GetBlob)

    .def("GetBools", &Property_GetBools)
    .def("GetInts", &Property_GetInts)
    .def("GetFloats", &Property_GetFloats)
    .def("GetStrings", &Property_GetStrings)
    .def("GetBlobs", &Property_GetBlobs)

    .def("GetValuesString", &luxrays::Property::GetValuesString)
    .def("ToString", &luxrays::Property::ToString)

    .def("Add", &Property_Add)
    .def("AddAllBool", &Property_AddAllBool)
    .def("AddAllInt", &Property_AddAllInt)
    .def("AddUnsignedLongLong", &Property_AddAllUnsignedLongLong)
    .def("AddAllFloat", &Property_AddAllFloat)
    .def("AddAllBool", &Property_AddAllBoolStride)
    .def("AddAllInt", &Property_AddAllIntStride)
    .def("AddUnsignedLongLong", &Property_AddAllUnsignedLongLongStride)
    .def("AddAllFloat", &Property_AddAllFloatStride)
    .def<PropertyRPtr (*)(PropertyRPtr , const py::list &)>
      ("Set", &Property_Set)
    .def<PropertyRPtr (*)(PropertyRPtr , const size_t, const py::object &)>
      ("Set", &Property_Set)

    .def("__str__", &luxrays::Property::ToString)
  ;

  //--------------------------------------------------------------------------
  // Properties class
  //--------------------------------------------------------------------------

  py::class_<luxrays::Properties, py::smart_holder>(m, "Properties")
    .def(
		py::init(
			[]() -> std::unique_ptr<luxrays::Properties>
			{ return std::make_unique<luxrays::Properties>(); }
		)
	)
    .def(
		py::init(
			[](std::string s) -> std::unique_ptr<luxrays::Properties>
			{ return std::make_unique<luxrays::Properties>(s); }
		)
	)
    .def(
		py::init(
			[](const Properties& props) -> std::unique_ptr<luxrays::Properties>
			{ return props.Clone(); }
		)
	)

    // Required because Properties::Set is overloaded
	.def<luxrays::Properties &(luxrays::Properties::*)(PropertyRPtr)>
      ("Set", &luxrays::Properties::Set)

    .def<luxrays::Properties &(luxrays::Properties::*)(const luxrays::Properties &)>
      ("Set", &luxrays::Properties::Set)

    .def<luxrays::Properties &(luxrays::Properties::*)(const luxrays::Properties &, const std::string &)>
      ("Set", &luxrays::Properties::Set)

    .def("SetFromFile", &luxrays::Properties::SetFromFile)
    .def("SetFromString", &luxrays::Properties::SetFromString)

    .def("Clear", &luxrays::Properties::Clear)
    .def("GetAllNamesRE", &luxrays::Properties::GetAllNamesRE)
    .def(
		"GetAllNames",
		py::overload_cast<>( &luxrays::Properties::GetAllNames, py::const_)
	)
    .def(
		"GetAllNames",
		py::overload_cast<const std::string &>(
			&luxrays::Properties::GetAllNames, py::const_
		)
	)
    .def(
		"GetAllUniqueSubNames",
		&luxrays::Properties::GetAllUniqueSubNames,
		"Get all unique subnames given a prefix",
		py::arg("prefix"),
		py::arg("sorted")=false
	)
    .def("HaveNames", &luxrays::Properties::HaveNames)
    .def("HaveNamesRE", &luxrays::Properties::HaveNamesRE)
    .def("GetAllProperties", &luxrays::Properties::GetAllProperties)

    .def<const luxrays::Property (luxrays::Properties::*)(const std::string &) const>
      ("Get", &luxrays::Properties::Get)
    .def("Get", &Properties_GetWithDefaultValues)

    .def("GetSize", &luxrays::Properties::GetSize)

    .def("IsDefined", &luxrays::Properties::IsDefined)
    .def("Delete", &luxrays::Properties::Delete)
    .def("DeleteAll", &luxrays::Properties::DeleteAll)
    .def("ToString", &luxrays::Properties::ToString)

    .def("__str__", &luxrays::Properties::ToString)
  ;

  //--------------------------------------------------------------------------
  // Film class
  //--------------------------------------------------------------------------

  py::enum_<Film::FilmOutputType>(m, "FilmOutputType")
    .value("RGB", Film::OUTPUT_RGB)
    .value("RGBA", Film::OUTPUT_RGBA)
    // RGB_TONEMAPPED is deprecated
    .value("RGB_TONEMAPPED", Film::OUTPUT_RGB_IMAGEPIPELINE)
    .value("RGB_IMAGEPIPELINE", Film::OUTPUT_RGB_IMAGEPIPELINE)
    // RGBA_TONEMAPPED is deprecated
    .value("RGBA_TONEMAPPED", Film::OUTPUT_RGBA_IMAGEPIPELINE)
    .value("RGBA_IMAGEPIPELINE", Film::OUTPUT_RGBA_IMAGEPIPELINE)
    .value("ALPHA", Film::OUTPUT_ALPHA)
    .value("DEPTH", Film::OUTPUT_DEPTH)
    .value("POSITION", Film::OUTPUT_POSITION)
    .value("GEOMETRY_NORMAL", Film::OUTPUT_GEOMETRY_NORMAL)
    .value("SHADING_NORMAL", Film::OUTPUT_SHADING_NORMAL)
    .value("MATERIAL_ID", Film::OUTPUT_MATERIAL_ID)
    .value("DIRECT_DIFFUSE", Film::OUTPUT_DIRECT_DIFFUSE)
    .value("DIRECT_DIFFUSE_REFLECT", Film::OUTPUT_DIRECT_DIFFUSE_REFLECT)
    .value("DIRECT_DIFFUSE_TRANSMIT", Film::OUTPUT_DIRECT_DIFFUSE_TRANSMIT)
    .value("DIRECT_GLOSSY", Film::OUTPUT_DIRECT_GLOSSY)
    .value("DIRECT_GLOSSY_REFLECT", Film::OUTPUT_DIRECT_GLOSSY_REFLECT)
    .value("DIRECT_GLOSSY_TRANSMIT", Film::OUTPUT_DIRECT_GLOSSY_TRANSMIT)
    .value("EMISSION", Film::OUTPUT_EMISSION)
    .value("INDIRECT_DIFFUSE", Film::OUTPUT_INDIRECT_DIFFUSE)
    .value("INDIRECT_DIFFUSE_REFLECT", Film::OUTPUT_INDIRECT_DIFFUSE_REFLECT)
    .value("INDIRECT_DIFFUSE_TRANSMIT", Film::OUTPUT_INDIRECT_DIFFUSE_TRANSMIT)
    .value("INDIRECT_GLOSSY", Film::OUTPUT_INDIRECT_GLOSSY)
    .value("INDIRECT_GLOSSY_REFLECT", Film::OUTPUT_INDIRECT_GLOSSY_REFLECT)
    .value("INDIRECT_GLOSSY_TRANSMIT", Film::OUTPUT_INDIRECT_GLOSSY_TRANSMIT)
    .value("INDIRECT_SPECULAR", Film::OUTPUT_INDIRECT_SPECULAR)
    .value("INDIRECT_SPECULAR_REFLECT", Film::OUTPUT_INDIRECT_SPECULAR_REFLECT)
    .value("INDIRECT_SPECULAR_TRANSMIT", Film::OUTPUT_INDIRECT_SPECULAR_TRANSMIT)
    .value("MATERIAL_ID_MASK", Film::OUTPUT_MATERIAL_ID_MASK)
    .value("DIRECT_SHADOW_MASK", Film::OUTPUT_DIRECT_SHADOW_MASK)
    .value("INDIRECT_SHADOW_MASK", Film::OUTPUT_INDIRECT_SHADOW_MASK)
    .value("RADIANCE_GROUP", Film::OUTPUT_RADIANCE_GROUP)
    .value("UV", Film::OUTPUT_UV)
    .value("RAYCOUNT", Film::OUTPUT_RAYCOUNT)
    .value("BY_MATERIAL_ID", Film::OUTPUT_BY_MATERIAL_ID)
    .value("IRRADIANCE", Film::OUTPUT_IRRADIANCE)
    .value("OBJECT_ID", Film::OUTPUT_OBJECT_ID)
    .value("OBJECT_ID_MASK", Film::OUTPUT_OBJECT_ID_MASK)
    .value("BY_OBJECT_ID", Film::OUTPUT_BY_OBJECT_ID)
    .value("SAMPLECOUNT", Film::OUTPUT_SAMPLECOUNT)
    .value("CONVERGENCE", Film::OUTPUT_CONVERGENCE)
    .value("SERIALIZED_FILM", Film::OUTPUT_SERIALIZED_FILM)
    .value("MATERIAL_ID_COLOR", Film::OUTPUT_MATERIAL_ID_COLOR)
    .value("ALBEDO", Film::OUTPUT_ALBEDO)
    .value("AVG_SHADING_NORMAL", Film::OUTPUT_AVG_SHADING_NORMAL)
    .value("NOISE", Film::OUTPUT_NOISE)
    .value("USER_IMPORTANCE", Film::OUTPUT_USER_IMPORTANCE)
    .value("CAUSTIC", Film::OUTPUT_CAUSTIC)
    .value("VARIANCE", Film::OUTPUT_VARIANCE)
    .value("MOTION_VECTOR", Film::OUTPUT_MOTION_VECTOR)
    .value("CRYPTOMATTE_OBJECT", Film::OUTPUT_CRYPTOMATTE_OBJECT)
    .value("CRYPTOMATTE_MATERIAL", Film::OUTPUT_CRYPTOMATTE_MATERIAL)
    .value("LPE", Film::OUTPUT_LPE)
    .def_property_readonly_static("names", [](py::object self){
        return self.attr("__members__");
    })
  ;

  //py::class_<luxcore::detail::FilmImpl, FilmImpl &>(m, "Film")
  py::class_<luxcore::detail::FilmImpl, py::smart_holder>(m, "Film")
    .def(
		py::init(
			[](std::string s) -> FilmImplUPtr {
				return luxcore::detail::FilmImpl::Create(s);
			}
		)
	)
    .def(
		py::init(
			[] (
				luxrays::PropertiesRPtr props,
				bool hasPixelNormalizedChannel,
				bool hasScreenNormalizedChannel
			) -> FilmImplUPtr {
				return luxcore::detail::FilmImpl::Create(
					props, hasPixelNormalizedChannel, hasScreenNormalizedChannel
				);
			}
		)
	)
    //.def(py::init<std::string>([](std::string s){ return luxcore::detail::FilmImpl::Create(s);})
    //.def(py::init<luxrays::Properties, bool, bool>())
    .def("GetWidth", &luxcore::detail::FilmImpl::GetWidth)
    .def("GetHeight", &luxcore::detail::FilmImpl::GetHeight)
    .def("GetStats", &luxcore::detail::FilmImpl::GetStats)
    .def("GetFilmY", &Film_GetFilmY1)
    .def("GetFilmY", &Film_GetFilmY2)
    .def("Save", &luxcore::detail::FilmImpl::SaveOutputs) // Deprecated
    .def("Clear", &luxcore::detail::FilmImpl::Clear)
    .def("AddFilm", &Film_AddFilm1)
    .def("AddFilm", &Film_AddFilm2)
    .def("HasOutput", &luxcore::detail::FilmImpl::HasOutput)
    .def("GetOutputCount", &luxcore::detail::FilmImpl::GetOutputCount)
    .def("SaveOutputs", &luxcore::detail::FilmImpl::SaveOutputs)
    .def("SaveOutput", &luxcore::detail::FilmImpl::SaveOutput)
    .def("SaveFilm", &luxcore::detail::FilmImpl::SaveFilm)
    .def("GetRadianceGroupCount", &luxcore::detail::FilmImpl::GetRadianceGroupCount)
    .def("GetOutputSize", &luxcore::detail::FilmImpl::GetOutputSize)
    .def("GetOutputFloatSnapshot", &Film_GetOutputFloatSnapshot, py::arg("output_type"),
        py::arg("buffer"), py::arg("index") = 0, py::arg("execute_imagepipeline") = true)
    .def("GetOutputFloat", &Film_GetOutputFloat1)
    .def("GetOutputFloat", &Film_GetOutputFloat2)
    .def("GetOutputFloat", &Film_GetOutputFloat3)
    .def("GetOutputUInt", &Film_GetOutputUInt1)
    .def("GetOutputUInt", &Film_GetOutputUInt2)
    .def("GetOutputUInt", &Film_GetOutputUInt3)
    .def("UpdateOutputFloat", &Film_UpdateOutputFloat1)
    .def("UpdateOutputFloat", &Film_UpdateOutputFloat2)
    .def("UpdateOutputFloat", &Film_UpdateOutputFloat3)
    .def("UpdateOutputUInt", &Film_UpdateOutputUInt1)
    .def("UpdateOutputUInt", &Film_UpdateOutputUInt2)
    .def("UpdateOutputUInt", &Film_UpdateOutputUInt3)
    .def("Parse", &luxcore::detail::FilmImpl::Parse)
    .def("DeleteAllImagePipelines", &luxcore::detail::FilmImpl::DeleteAllImagePipelines)
    .def("ExecuteImagePipeline", &luxcore::detail::FilmImpl::ExecuteImagePipeline)
    .def("AsyncExecuteImagePipeline", &luxcore::detail::FilmImpl::AsyncExecuteImagePipeline)
    .def("HasDoneAsyncExecuteImagePipeline", &luxcore::detail::FilmImpl::HasDoneAsyncExecuteImagePipeline)
    .def("WaitAsyncExecuteImagePipeline", &luxcore::detail::FilmImpl::WaitAsyncExecuteImagePipeline)
	.def("ApplyOIDN", &Film_ApplyOIDN,
		py::call_guard<py::gil_scoped_release>())
  ;

  //--------------------------------------------------------------------------
  // Camera class
  //--------------------------------------------------------------------------

  py::class_<luxcore::detail::CameraImpl, py::smart_holder>(m, "Camera")
    .def("Translate", &Camera_Translate)
    .def("TranslateLeft", &luxcore::detail::CameraImpl::TranslateLeft)
    .def("TranslateRight", &luxcore::detail::CameraImpl::TranslateRight)
    .def("TranslateForward", &luxcore::detail::CameraImpl::TranslateForward)
    .def("TranslateBackward", &luxcore::detail::CameraImpl::TranslateBackward)
    .def("Rotate", &Camera_Rotate)
    .def("RotateLeft", &luxcore::detail::CameraImpl::RotateLeft)
    .def("RotateRight", &luxcore::detail::CameraImpl::RotateRight)
    .def("RotateUp", &luxcore::detail::CameraImpl::RotateUp)
    .def("RotateDown", &luxcore::detail::CameraImpl::RotateDown)
  ;

  //--------------------------------------------------------------------------
  // Scene class
  //--------------------------------------------------------------------------

  using luxcore::detail::SceneImpl;
  py::class_<SceneImpl, py::smart_holder>(m, "Scene")
	.def(
		py::init(&SceneImpl::Create<slg::SceneRef>),
		py::keep_alive<1, 2>()
	)
	.def(
		py::init(&SceneImpl::Create<slg::SceneRef>),
		py::keep_alive<1, 2>()
	)
    .def(
		py::init(&SceneImpl::Create<luxrays::PropertiesRPtr, luxrays::PropertiesRPtr>),
		py::keep_alive<1, 2>(),
		py::keep_alive<1, 3>(),
		py::call_guard<py::gil_scoped_release>()
	)
    .def(
		py::init(&SceneImpl::Create<luxrays::PropertiesRPtr>),
		py::arg("resizePolicyProps") = nullptr,
		py::keep_alive<1, 2>(),
		py::call_guard<py::gil_scoped_release>()
	)
    .def(
		py::init(&SceneImpl::Create<std::string>),
		py::keep_alive<1, 2>(),
		py::call_guard<py::gil_scoped_release>()
	)
    .def("ToProperties", &luxcore::detail::SceneImpl::ToProperties)
    .def("GetCamera", &Scene_GetCamera)
    .def("GetLightCount", &luxcore::detail::SceneImpl::GetLightCount)
    .def("GetObjectCount", &luxcore::detail::SceneImpl::GetObjectCount)
    // The scene mutation calls below all release the GIL: BlendLuxCore's
    // viewport runs them on a session worker thread and a held GIL would
    // freeze Blender's UI thread for the whole call (mesh uploads, image
    // loading in Parse(), instancing duplication). Wrappers taking py::
    // arguments convert their inputs under the GIL first, then release it
    // inside right before the SceneImpl call; pure C++ signatures use
    // call_guard<gil_scoped_release> on the binding instead.
    .def("DefineImageMap", &Scene_DefineImageMap)
    .def("IsImageMapDefined", &luxcore::detail::SceneImpl::IsImageMapDefined)
    .def("DefineMesh", &Scene_DefineMesh1,
		py::arg("name"), py::arg("points"), py::arg("triangles"),
		py::arg("normals") = py::none(), py::arg("uvs") = py::none(),
		py::arg("colors") = py::none(), py::arg("alphas") = py::none(),
		py::arg("transformation") = py::none(),
		py::arg("generated_transformation") = py::none())
    .def(
		"DefineMeshExt",
		&Scene_DefineMeshExt3,
		"Define an extended mesh from Numpy arrays",
		py::arg("name"),
		py::arg("points"),
		py::arg("triangles"),
		py::arg("normals") = py::none(),
		py::arg("uvs") = py::none(),
		py::arg("colors") = py::none(),
		py::arg("alphas") = py::none(),
		py::arg("transformation") = py::none(),
		py::arg("generated_transformation") = py::none()
	)
	.def("DefineMeshExt", &Scene_DefineMeshExt1,
		py::arg("name"), py::arg("points"), py::arg("triangles"),
		py::arg("normals") = py::none(), py::arg("uvs") = py::none(),
		py::arg("colors") = py::none(), py::arg("alphas") = py::none(),
		py::arg("transformation") = py::none(),
		py::arg("generated_transformation") = py::none())
    .def("SetMeshVertexAOV", &Scene_SetMeshVertexAOV)
    .def("SetMeshTriangleAOV", &Scene_SetMeshTriangleAOV)
    .def("SetMeshVertexMotion", &Scene_SetMeshVertexMotion)
    .def("SetStrandsVertexMotion", &Scene_SetStrandsVertexMotion)
    .def("SetMeshAppliedTransformation", &Scene_SetMeshAppliedTransformation)
    .def("SaveMesh", &luxcore::detail::SceneImpl::SaveMesh)
    .def("SaveMeshClusterStride", &luxcore::detail::SceneImpl::SaveMeshClusterStride)
    .def("DefineStrands", &Scene_DefineStrands)
    .def("DefineBlenderStrands", &blender::Scene_DefineBlenderStrands)
    .def("DefineBlenderCurveStrands", &blender::Scene_DefineBlenderCurveStrands)
    .def("IsMeshDefined", &luxcore::detail::SceneImpl::IsMeshDefined)
    .def("IsTextureDefined", &luxcore::detail::SceneImpl::IsTextureDefined)
    .def("IsMaterialDefined", &luxcore::detail::SceneImpl::IsMaterialDefined)
    .def("Parse", &luxcore::detail::SceneImpl::Parse, py::keep_alive<1, 2>(),
         py::call_guard<py::gil_scoped_release>())
    .def("DuplicateObject", &Scene_DuplicateObject)
    .def("DuplicateObject", &Scene_DuplicateObjectMulti)
    .def("DuplicateObject", &Scene_DuplicateMotionObject)
    .def("DuplicateObject", &Scene_DuplicateMotionObjectMulti)
    .def("DeleteObjects", &Scene_DeleteObjects)
    .def("DeleteLights", &Scene_DeleteLights)
    .def("UpdateObjectTransformation", &Scene_UpdateObjectTransformation)
    .def("UpdateObjectMaterial", &luxcore::detail::SceneImpl::UpdateObjectMaterial,
         py::call_guard<py::gil_scoped_release>())
    .def("SetObjectSubsurfaceGroup", &luxcore::detail::SceneImpl::SetObjectSubsurfaceGroup,
         py::call_guard<py::gil_scoped_release>())
    .def("DeleteObject", &luxcore::detail::SceneImpl::DeleteObject,
         py::call_guard<py::gil_scoped_release>())
    .def("DeleteLight", &luxcore::detail::SceneImpl::DeleteLight,
         py::call_guard<py::gil_scoped_release>())
    .def("RemoveUnusedImageMaps", &luxcore::detail::SceneImpl::RemoveUnusedImageMaps,
         py::call_guard<py::gil_scoped_release>())
    .def("RemoveUnusedTextures", &luxcore::detail::SceneImpl::RemoveUnusedTextures,
         py::call_guard<py::gil_scoped_release>())
    .def("RemoveUnusedMaterials", &luxcore::detail::SceneImpl::RemoveUnusedMaterials,
         py::call_guard<py::gil_scoped_release>())
    .def("RemoveUnusedMeshes", &luxcore::detail::SceneImpl::RemoveUnusedMeshes,
         py::call_guard<py::gil_scoped_release>())
    .def("Save", &luxcore::detail::SceneImpl::Save)
  ;

  //--------------------------------------------------------------------------
  // RenderConfig class
  //--------------------------------------------------------------------------

  py::class_<luxcore::detail::RenderConfigImpl, py::smart_holder>(m, "RenderConfig")
    // GIL released in the ctors: engine/device creation can compile the
    // intersection kernels and would otherwise freeze the calling UI thread.
	.def(
		py::init(&RenderConfigImpl::Create<luxrays::PropertiesRPtr>),
		py::keep_alive<1, 2>(),
        py::call_guard<py::gil_scoped_release>()
	)
    .def(
		py::init(&RenderConfigImpl::Create<luxrays::PropertiesRPtr, SceneImpl&>),
		py::keep_alive<1, 2>(),
		py::keep_alive<1, 3>(),
        py::call_guard<py::gil_scoped_release>()
	)
	// Load a serialized (binary) RenderConfig saved with
	// RenderConfig.Save() — used by the external render process.
    .def(
		py::init(&RenderConfig_LoadFile),
        py::call_guard<py::gil_scoped_release>()
	)
    // GetProperties() returns the internal config by const unique_ptr&
    // (non-owning). Under py::smart_holder a unique_ptr& return can only be
    // materialized by aliasing the parent's shared holder — which fails with
    // "Non-owning holder (load_as_shared_ptr)" when the RenderConfig wrapper
    // itself is the non-owning one returned by RenderSession.GetRenderConfig().
    // Clone the Properties so the result is always owned.
    .def("GetProperties", [](const luxcore::detail::RenderConfigImpl &self) {
        return self.GetProperties()->Clone();
    })
    .def("GetProperty", &luxcore::detail::RenderConfigImpl::GetProperty)
    // A newly wrapped internal scene borrows its config. An external scene
    // already has an owning Python wrapper retained by the config constructor.
    // reference_internal only attaches the parent for a NEW wrapper; an
    // unconditional keep_alive would create scene -> config -> scene cycles.
    .def("GetScene", &RenderConfig_GetScene,
         py::return_value_policy::reference_internal)
    .def("HasCachedKernels", &luxcore::detail::RenderConfigImpl::HasCachedKernels)
    .def("Parse", &luxcore::detail::RenderConfigImpl::Parse)
    .def("Delete", &luxcore::detail::RenderConfigImpl::Delete)
    .def("GetFilmSize", &RenderConfig_GetFilmSize)
    .def("Save", &luxcore::detail::RenderConfigImpl::Save)
    .def("Export", &luxcore::detail::RenderConfigImpl::Export)
    .def_static("LoadResumeFile", &RenderConfig_LoadResumeFile)
    .def_static("GetDefaultProperties", &luxcore::detail::RenderConfigImpl::GetDefaultProperties)
  ;

  //--------------------------------------------------------------------------
  // RenderState class
  //--------------------------------------------------------------------------

  py::class_<luxcore::detail::RenderStateImpl, py::smart_holder>(m, "RenderState")
    .def("Save", &RenderState::Save)
  ;

  //--------------------------------------------------------------------------
  // RenderSession class
  //--------------------------------------------------------------------------

  py::class_<luxcore::detail::RenderSessionImpl, py::smart_holder>(m, "RenderSession")
	.def(
		py::init<>(&RenderSessionImpl::Create<luxcore::detail::RenderConfigImpl&>),
		py::keep_alive<1, 2>(),
		py::call_guard<py::gil_scoped_release>()
	)

	.def(
		py::init<>(&RenderSessionImpl::Create
			<RenderConfigImplRef, std::string&, std::string&>
		),
		py::keep_alive<1, 2>(),
		py::call_guard<py::gil_scoped_release>()
	)

	.def(
		py::init<>(&RenderSessionImpl::Create
			<RenderConfigImpl&, RenderStateImplRPtr&, FilmImpl& >
		),
		py::keep_alive<1, 2>(),
		// The engine holds a non-owning FilmPtr to the start film and
		// dereferences it in Start() (film->AddFilm) - the film must
		// outlive the session
		py::keep_alive<1, 4>(),
		py::call_guard<py::gil_scoped_release>()
	)
	//TODO
    //.def("GetRenderConfig", &RenderSession_GetRenderConfig)
    // Python session constructors already retain the owning config wrapper.
    // Reuse it without adding config -> session -> config keep-alive cycles.
    // A genuinely new borrowed wrapper would still retain its parent.
    .def("GetRenderConfig", &luxcore::detail::RenderSessionImpl::GetRenderConfig,
         py::return_value_policy::reference_internal)
    // Long running calls release the GIL: kernel compilation (i.e. the
    // Metal path is a multi-second cl2msl + Metal compiler run) happens
    // inside Start(), and WaitNewFrame()/WaitForDone()/Stop() block on
    // render threads. Holding the GIL there freezes every other Python
    // thread - including Blender's UI callbacks - for the whole duration.
    .def("Start", &luxcore::detail::RenderSessionImpl::Start,
         py::call_guard<py::gil_scoped_release>())
    .def("Stop", &luxcore::detail::RenderSessionImpl::Stop,
         py::call_guard<py::gil_scoped_release>())
    .def("IsStarted", &luxcore::detail::RenderSessionImpl::IsStarted)
    .def("BeginSceneEdit", &luxcore::detail::RenderSessionImpl::BeginSceneEdit,
         py::call_guard<py::gil_scoped_release>())
    .def("EndSceneEdit", &luxcore::detail::RenderSessionImpl::EndSceneEdit,
         py::call_guard<py::gil_scoped_release>())
    .def("IsInSceneEdit", &luxcore::detail::RenderSessionImpl::IsInSceneEdit)
    .def("Pause", &luxcore::detail::RenderSessionImpl::Pause,
         py::call_guard<py::gil_scoped_release>())
    .def("Resume", &luxcore::detail::RenderSessionImpl::Resume,
         py::call_guard<py::gil_scoped_release>())
    .def("IsInPause", &luxcore::detail::RenderSessionImpl::IsInPause)
    .def("AcknowledgeDenoisingFrame", &RenderSession_AcknowledgeDenoisingFrame,
         py::arg("frame"), py::arg("keep_lossless")=true)
    .def("ReleaseDenoisingFrameDelivery", &RenderSession_ReleaseDenoisingFrameDelivery)
    .def("SetDenoisingFrameHaltSPP", &RenderSession_SetDenoisingFrameHaltSPP,
         py::arg("samples"), py::call_guard<py::gil_scoped_release>(),
         "Update the coherent eye-sample cap without resetting the film (0 disables it); serialize with session lifecycle edits.")
    .def("GetDenoisingFrame", &RenderSession_GetDenoisingFrame,
         py::arg("after_id")=0, py::arg("timeout_ms")=0,
         "Lease the newest complete independent RTPATH GPU frame (or None). "
         "Read-only arrays remain valid across later edits, resize and stop; no UI/GPU queue drain.")
    .def("GetDenoisingGuides", &RenderSession_GetDenoisingGuides,
         "Read renderer-owned GPU primary-hit guides from a finished/paused frame. "
         "Requires film.denoisingguides.enable; serialize with session lifecycle edits.")
    .def("GetDenoisingSurfaceGuides", &RenderSession_GetDenoisingSurfaceGuides,
         py::arg("jitter_x") = 0.f, py::arg("jitter_y") = 0.f,
         "Optional host reference guides; serialize with session lifecycle edits. "
         "Requires a paused or finished frame. Status 2/3 is not ready for DLSS presentation.")
    .def("SetRuntimeResolutionReduction",
         &luxcore::detail::RenderSessionImpl::SetRuntimeResolutionReduction,
         "Runtime override of the RTPATHOCL resolution reduction (0 restores "
         "the configured value; applies at the next frame boundary, no film reset)")
    // The returned film is a FilmImplSession holding a non-owning
    // back-link to the session: keep the session alive while the
    // wrapper is in use (same as GetRenderConfig above).
    .def("GetFilm", &luxcore::detail::RenderSessionImpl::GetFilmPtr,
         py::keep_alive<0, 1>())
    .def("UpdateStats", &luxcore::detail::RenderSessionImpl::UpdateStats,
         py::call_guard<py::gil_scoped_release>())
    .def("GetStats", &luxcore::detail::RenderSessionImpl::GetStats)
    .def("WaitNewFrame", &luxcore::detail::RenderSessionImpl::WaitNewFrame,
         py::call_guard<py::gil_scoped_release>())
    .def("WaitForDone", &luxcore::detail::RenderSessionImpl::WaitForDone,
         py::call_guard<py::gil_scoped_release>())
    .def("HasDone", &luxcore::detail::RenderSessionImpl::HasDone)
    .def("Parse", &luxcore::detail::RenderSessionImpl::Parse,
         py::call_guard<py::gil_scoped_release>())
    .def("GetRenderState", &RenderSession_GetRenderState, py::return_value_policy::take_ownership)
    .def("SaveResumeFile", &luxcore::detail::RenderSessionImpl::SaveResumeFile,
         py::call_guard<py::gil_scoped_release>())
  ;

  m.def("GetOpenVDBGridNames", &GetOpenVDBGridNames);
  RegisterMikkTangents(m);
  m.def("GetOpenVDBGridInfo", &GetOpenVDBGridInfo,
      py::arg("file_path"), py::arg("grid_name"), py::arg("active_bounds") = false);
	m.def("BlenderMatrix4x4ToList", &blender::BlenderMatrix4x4ToList);


  // Note: used by pysuperluxcoredemo.py, do not remove.
  m.def("ConvertFilmChannelOutput_3xFloat_To_4xUChar", ConvertFilmChannelOutput_3xFloat_To_4xUChar);
}

}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
