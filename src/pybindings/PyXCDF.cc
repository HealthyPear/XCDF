/*!
 * @file PyXCDF.cc
 * @author Segev BenZvi
 * @date 30 May 2013
 * @brief Define python bindings to XCDF functions using the python C API.
 * @version $Id: PyXCDF.cc 18676 2014-02-03 01:39:00Z sybenzvi $
 */

#include <Python.h>

#include <xcdf/XCDFFile.h>
#include <xcdf/utility/EventSelectExpression.h>
#include <XCDFTypeConversion.h>
#include <XCDFHeaderVisitor.h>
#include <XCDFTupleSetter.h>
#include <XCDFFieldsByNameSelector.h>

#include <structmember.h>
#include <string.h>

#include <iomanip>
#include <sstream>

// ___________________________________
// Expose parts of XCDFFile to python \_________________________________________
typedef struct {
  PyObject_HEAD
  PyObject* filename_;    // XCDF filename (a python string)
  XCDFFile* file_;        // an XCDF file instance
} pyxcdf_XCDFFile;


// XCDFException object
PyObject* pyxcdf_XCDFException;


// Python XCDFFile allocator
static PyObject*
XCDFFile_new(PyTypeObject* type, PyObject* args, PyObject* kwds)
{
  pyxcdf_XCDFFile* self = (pyxcdf_XCDFFile*)type->tp_alloc(type, 0);
  if (self != NULL) {
    #if PY_MAJOR_VERSION >= 3
    self->filename_ = PyBytes_FromString("");
    #else
    self->filename_ = PyString_FromString("");
    #endif
    if (self->filename_ == NULL) {
      Py_DECREF(self);
      return NULL;
    }
    self->file_ = NULL;
  }
  return (PyObject*)self;
}

// Python XCDFFile initializer (from filename argument)
static int
XCDFFile_init(pyxcdf_XCDFFile* self, PyObject* args)
{
  PyObject* filename = NULL;
  PyObject* filemode = NULL;
  PyObject* tmp;

  static const char* format = "S|S";

  if (!PyArg_ParseTuple(args, format, &filename, &filemode))
    return -1;

  if (filename) {
    tmp = self->filename_;
    Py_INCREF(filename);
    self->filename_ = filename;
    Py_XDECREF(tmp);

    char mode[10];
    if (filemode) {
      #if PY_MAJOR_VERSION >= 3
      char* tmp = PyBytes_AsString(filemode);
      #else
      char* tmp = PyString_AsString(filemode);
      #endif
      strcpy(mode, tmp);
    }
    else
      strcpy(mode, "R");

    try {
      #if PY_MAJOR_VERSION >= 3
      self->file_ = new XCDFFile(PyBytes_AsString(filename), mode);
      #else
      self->file_ = new XCDFFile(PyString_AsString(filename), mode);
      #endif
    }
    catch (const XCDFException& e) {
      PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
      return -1;
    }
  }

  return 0;
}

// Python XCDFFile deallocator
static void
XCDFFile_dealloc(pyxcdf_XCDFFile* self)
{
  Py_XDECREF(self->filename_);
  if (self->file_)
    delete self->file_;
  #if PY_MAJOR_VERSION >= 3
  Py_TYPE(self)->tp_free((PyObject*)self);
  #else
  self->ob_type->tp_free((PyObject*)self);
  #endif
}

// Member definitions for XCDFFile object
static PyMemberDef XCDFFile_members[] =
{
  { const_cast<char*>("filename"), T_OBJECT_EX,
    offsetof(pyxcdf_XCDFFile, filename_), 0,
    const_cast<char*>("XCDF file name") },

  { NULL }
};

// _______________________
// XCDFFile data iterator \_____________________________________________________
typedef std::vector<XCDFFieldDescriptor>::const_iterator XCDFDescriptorIterator;

typedef struct {
  PyObject_HEAD
  XCDFFile* file_;                      // pointer to current open XCDF file
  int iCurrent_;                        // current record being read
  int iTotal_;                          // total number of records in file
  FieldsByNameSelector* selectField_;   // Field extraction object
  EventSelectExpression selectEvent_;   // object indicating whether an event
                                        //      should be selected or skipped
} XCDFRecordIterator;

// Define __init__
static int
XCDFRecordIterator_init(XCDFRecordIterator* self, PyObject* args) {

  if (self->selectField_) {
    delete self->selectField_;
  }
  self->selectField_ = NULL;
  self->file_ = NULL;
  self->iCurrent_ = self->iTotal_ = 0;
  return 0;
}

// Define __del__
static void
XCDFRecordIterator_dealloc(XCDFRecordIterator* self) {
  if (self->selectField_) {
    delete self->selectField_;
  }
  #if PY_MAJOR_VERSION >= 3
  Py_TYPE(self)->tp_free((PyObject*)self);
  #else
  self->ob_type->tp_free((PyObject*)self);
  #endif
}

// Define __iter__()
PyObject*
XCDFRecordIterator_iter(PyObject* self)
{
  Py_INCREF(self);
  return self;
}

// Define next() for iteration over XCDF records
PyObject*
XCDFRecordIterator_iternext(PyObject* self)
{
  XCDFRecordIterator* p = (XCDFRecordIterator*)self;

  try {

    for (;;) {

      // If we have not reached the end of the file:
      if (p->iCurrent_ < p->iTotal_ && p->file_->Read() > 0)
      {
        p->iCurrent_ = p->file_->GetCurrentEventNumber();
        if (p->iCurrent_ < 0) {
          PyErr_SetNone(PyExc_StopIteration);
          return NULL;
        }

        // Check if we should select this event
        if (!p->selectEvent_.SelectEvent()) {
          continue;
        }

        if (p->selectField_) {

          // We have a list of fields to extract
          return p->selectField_->GetTuple();
        } else {

          // Keep all fields.
          // Create a field visitor to stuff data into a tuple
          TupleSetter tsetter(p->file_->GetNFields());
          p->file_->ApplyFieldVisitor(tsetter);

          PyObject* result(tsetter.GetTuple());
          return result;
        }
      }
      // When reaching EOF, rewind the XCDF file and stop the iterator
      else {
        p->file_->Rewind();
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
      }
    }
  } catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    return NULL;
  }

  // Should never be reached
  return NULL;
}

// Definition of the record iterator type for python
static PyTypeObject
XCDFRecordIteratorType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyxcdf._iter",                             // tp_name
    sizeof(XCDFRecordIterator),                 // tp_basicsize
    0,                                          // tp_itemsize
    (destructor)XCDFRecordIterator_dealloc,     // tp_dealloc
    0,                                          // tp_print
    0,                                          // tp_getattr
    0,                                          // tp_setattr
    0,                                          // tp_reserved
    0,                                          // tp_repr
    0,                                          // tp_as_number
    0,                                          // tp_as_sequence
    0,                                          // tp_as_mapping
    0,                                          // tp_hash
    0,                                          // tp_call
    0,                                          // tp_str
    0,                                          // tp_getattro
    0,                                          // tp_setattro
    0,                                          // tp_as_buffer
    #if PY_MAJOR_VERSION >= 3
    Py_TPFLAGS_DEFAULT,                         // use tp_iter and tp_iternext
    #else
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_ITER,  // use tp_iter and tp_iternext
    #endif
    "Internal record iterator object",          // tp_doc
    0,                                          // tp_traverse
    0,                                          // tp_clear
    0,                                          // tp_richcompare
    0,                                          // tp_weaklistoffset
    (getiterfunc)XCDFRecordIterator_iter,       // tp_iter
    (iternextfunc)XCDFRecordIterator_iternext,  // tp_iternext
    0,                                          // tp_methods
    0,                                          // tp_members
    0,                                          // tp_getset
    0,                                          // tp_base
    0,                                          // tp_dict
    0,                                          // tp_descr_get
    0,                                          // tp_descr_set
    0,                                          // tp_dictoffset
    (initproc)XCDFRecordIterator_init,          // tp_init
    0,                                          // tp_alloc
    PyType_GenericNew,                          // tp_new
};

// ________________________
// XCDFFile field iterator \____________________________________________________
typedef struct {
  PyObject_HEAD
  XCDFFile* file_;                      // pointer to current open XCDF file
  int iCurrent_;                        // current record being read
  int iTotal_;                          // total number of records in file
  FieldsByNameSelector* selector_;      // Field extraction object
} XCDFFieldIterator;

// Define __init__
static int
XCDFFieldIterator_init(XCDFFieldIterator* self, PyObject* args) {

  if (self->selector_) {
    delete self->selector_;
  }
  self->selector_ = NULL;
  self->file_ = NULL;
  self->iCurrent_ = self->iTotal_ = 0;
  return 0;
}

// Define __del__
static void
XCDFFieldIterator_dealloc(XCDFFieldIterator* self) {
  if (self->selector_) {
    delete self->selector_;
  }
  #if PY_MAJOR_VERSION >= 3
  PyTYPE(self)->tp_free((PyObject*)self);
  #else
  self->ob_type->tp_free((PyObject*)self);
  #endif
}

// Define __iter__()
PyObject*
XCDFFieldIterator_iter(PyObject* self)
{
  Py_INCREF(self);
  return self;
}

// Define next() for iteration over XCDF fields
PyObject*
XCDFFieldIterator_iternext(PyObject* self)
{
  XCDFFieldIterator* p = (XCDFFieldIterator*)self;

  try {

    // If we have not reached the end of the file:
    if (p->iCurrent_ < p->iTotal_ && p->file_->Read() > 0)
    {
      p->iCurrent_ = p->file_->GetCurrentEventNumber();
      if (p->iCurrent_ < 0) {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
      }
      return p->selector_->GetTuple();
    }
    // When reaching EOF, rewind the XCDF file and stop the iterator
    else {
      p->file_->Rewind();
      PyErr_SetNone(PyExc_StopIteration);
      return NULL;
    }
  } catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    return NULL;
  }
}

// Definition of the field iterator type for python
static PyTypeObject
XCDFFieldIteratorType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyxcdf._iter",                             // tp_name
    sizeof(XCDFFieldIterator),                  // tp_basicsize
    0,                                          // tp_itemsize
    (destructor)XCDFFieldIterator_dealloc,      // tp_dealloc
    0,                                          // tp_print
    0,                                          // tp_getattr
    0,                                          // tp_setattr
    0,                                          // tp_reserved
    0,                                          // tp_repr
    0,                                          // tp_as_number
    0,                                          // tp_as_sequence
    0,                                          // tp_as_mapping
    0,                                          // tp_hash
    0,                                          // tp_call
    0,                                          // tp_str
    0,                                          // tp_getattro
    0,                                          // tp_setattro
    0,                                          // tp_as_buffer
    #if PY_MAJOR_VERSION >= 3
    Py_TPFLAGS_DEFAULT,                         // use tp_iter and tp_iternext
    #else
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_ITER,  // use tp_iter and tp_iternext
    #endif
    "Internal field iterator object",           // tp_doc
    0,                                          // tp_traverse
    0,                                          // tp_clear
    0,                                          // tp_richcompare
    0,                                          // tp_weaklistoffset
    (getiterfunc)XCDFFieldIterator_iter,        // tp_iter
    (iternextfunc)XCDFFieldIterator_iternext,   // tp_iternext
    0,                                          // tp_methods
    0,                                          // tp_members
    0,                                          // tp_getset
    0,                                          // tp_base
    0,                                          // tp_dict
    0,                                          // tp_descr_get
    0,                                          // tp_descr_set
    0,                                          // tp_dictoffset
    (initproc)XCDFFieldIterator_init,           // tp_init
    0,                                          // tp_alloc
    PyType_GenericNew,                          // tp_new
};

// __________________________
// XCDFFile member functions \__________________________________________________

// Function to print header information from the file
static PyObject*
XCDFFile_header(pyxcdf_XCDFFile* self)
{
  // Make sure the XCDF file is valid
  if (self->file_ == NULL) {
    PyErr_SetString(PyExc_AttributeError, "file: not open");
    return NULL;
  }

  try {
    // Create a field visitor to stuff header data into a string buffer
    std::stringstream ostr;

    HeaderVisitor hvisitor(*(self->file_), ostr);
    self->file_->ApplyFieldVisitor(hvisitor);

    #if PY_MAJOR_VERSION >= 3
    PyObject* result = PyBytes_FromString(ostr.str().c_str());
    #else
    PyObject* result = PyString_FromString(ostr.str().c_str());
    #endif
    return result;
  }
  catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    return NULL;
  }
}

// Function to set up access to the record iterator
static PyObject*
XCDFRecord_iterator(pyxcdf_XCDFFile* self, PyObject* args, PyObject* kwargs)
{
  // Make sure the XCDF file is valid
  if (self->file_ == NULL) {
    PyErr_SetString(PyExc_AttributeError, "file: not open");
    return NULL;
  }

  PyObject* select = NULL;
  PyObject* fields = NULL;
  char *kwlist[] = {const_cast<char*>("select"),
                    const_cast<char*>("fields"),
                    NULL};

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwargs,
                                   "|OO",
                                   kwlist,
                                   &select,
                                   &fields)) {

    PyErr_SetString(PyExc_TypeError, "records([select=\"expression\"])");
    return NULL;
  }

  // By default, select all events
  const char* selectExpression = "true";
  if (select) {
    // Use the user-provided event selection
    #if PY_MAJOR_VERSION >= 3
    selectExpression = PyBytes_AsString(select);
    #else
    selectExpression = PyString_AsString(select);
    #endif
  }

  XCDFRecordIterator* it = NULL;
  try {
    it = (XCDFRecordIterator*)
           PyObject_CallObject((PyObject*)&XCDFRecordIteratorType, NULL);

    if (!it) {
      std::cerr << "Record iterator is NULL" << std::endl;
      return NULL;
    }

    it->file_ = self->file_;
    it->iCurrent_ = 0;
    it->iTotal_ = self->file_->GetEventCount();
    it->selectEvent_ = EventSelectExpression(selectExpression, *(self->file_));
    if (fields) {

      #if PY_MAJOR_VERSION >= 3
      it->selectField_ = new FieldsByNameSelector(
                             PyBytes_AsString(fields), *(self->file_));
      #else
      it->selectField_ = new FieldsByNameSelector(
                             PyString_AsString(fields), *(self->file_));
      #endif
    }

    return (PyObject*)it;
  }
  catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    Py_XDECREF(it);
    return NULL;
  }
}

// Function to set up access to field iterator
static PyObject*
XCDFField_iterator(pyxcdf_XCDFFile* self, PyObject* fieldName)
{
  // Make sure the XCDF file is valid
  if (self->file_ == NULL) {
    PyErr_SetString(PyExc_AttributeError, "file: not open");
    return NULL;
  }

  XCDFFieldIterator* it = NULL;
  try {
    it = (XCDFFieldIterator*)
           PyObject_CallObject((PyObject*)&XCDFFieldIteratorType, NULL);

    if (!it) {
      std::cerr << "Field iterator is NULL" << std::endl;
      return NULL;
    }

    it->file_ = self->file_;
    it->iCurrent_ = 0;
    it->iTotal_ = self->file_->GetEventCount();
    #if PY_MAJOR_VERSION >= 3
    it->selector_ = new FieldsByNameSelector(
                         PyBytesAsString(fieldName), *(self->file_));
    #else
    it->selector_ = new FieldsByNameSelector(
                         PyString_AsString(fieldName), *(self->file_));
    #endif
    return (PyObject*)it;
  }
  catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    Py_XDECREF(it);
    return NULL;
  }
}

// Function to provide random access to a record in the file
static PyObject*
XCDFFile_getRecord(pyxcdf_XCDFFile* self, PyObject* args)
{

  // Parse the arguments
  PyObject* recordId = NULL;
  PyObject* fields = NULL;
  if (!PyArg_ParseTuple(args, "O|O", &recordId, &fields)) {
    PyErr_SetString(PyExc_TypeError, "getRecord(number [field CSV list])");
    return NULL;
  }


  // Make sure the XCDF file is valid
  if (self->file_ == NULL) {
    PyErr_SetString(PyExc_AttributeError, "file: not open");
    return NULL;
  }

  PyObject* result = NULL;
  try {
    // Seek to a given record ID in the file
    #if PY_MAJOR_VERSION >= 3
    uint64_t id = PyLong_AsUnsignedLongLongMask(recordId);
    #else
    uint64_t id = PyInt_AsUnsignedLongLongMask(recordId);
    #endif

    if (self->file_->Seek(id)) {

      if (fields == NULL) {

        // Copy all fields.  Create a field visitor to stuff data into a tuple
        TupleSetter tsetter(self->file_->GetNFields());
        self->file_->ApplyFieldVisitor(tsetter);
        result = tsetter.GetTuple();
      } else {

        // Only copy the selected fields
        #if PY_MAJOR_VERSION >= 3
        FieldsByNameSelector selector =
            FieldsByNameSelector(PyBytes_AsString(fields), *(self->file_));
        #else
        FieldsByNameSelector selector =
            FieldsByNameSelector(PyString_AsString(fields), *(self->file_));
        #endif
        result = selector.GetTuple();
      }

      self->file_->Rewind();
    }
    else {
      std::stringstream errMsg;
      errMsg << "Invalid event number " << id;
      PyErr_SetString(pyxcdf_XCDFException, errMsg.str().c_str());
    }

    return result;
  }
  catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    Py_XDECREF(result);
    return NULL;
  }
}

static PyObject*
XCDFFile_addField(pyxcdf_XCDFFile* self, PyObject* args)
{
  // Make sure the XCDF file is valid
  if (self->file_ == NULL) {
    PyErr_SetString(PyExc_AttributeError, "file: not open");
    return NULL;
  }

  try {
    PyObject *name = NULL;      // Field name
    PyObject *type = NULL;      // Field type (XCDF (un)signed integer/float)
    PyObject *reso = NULL;      // Field resolution
    PyObject *pnam = NULL;      // Parent name (if vector)

    static const char *format = "OOO|O:addField";

    if (!PyArg_ParseTuple(args, format, &name, &type, &reso, &pnam)) {
      PyErr_SetString(PyExc_TypeError, "addField(name, type, resolution, "
                                       "[parent name])");
      return NULL;
    }

    #if PY_MAJOR_VERSION >= 3
    char *nameStr = PyBytes_AsString(name);
    XCDFFieldType ftype = static_cast<XCDFFieldType>(PyLong_AsLong(type));
    #else
    char *nameStr = PyString_AsString(name);
    XCDFFieldType ftype = static_cast<XCDFFieldType>(PyInt_AsLong(type));
    #endif

    char parent[80];
    if (pnam)
      #if PY_MAJOR_VERSION >= 3
      strcpy(parent, PyBytes_AsString(pnam));
      #else
      strcpy(parent, PyString_AsString(pnam));
      #endif
    else
      strcpy(parent, "");

    switch (ftype) {
      case XCDF_UNSIGNED_INTEGER:
      {
        #if PY_MAJOR_VERSION >= 3
        uint64_t res = PyLong_AsUnsignedLongLongMask(reso);
        #else
        uint64_t res = PyInt_AsUnsignedLongLongMask(reso);
        #endif
        self->file_->AllocateUnsignedIntegerField(nameStr, res, parent);
        break;
      }
      case XCDF_SIGNED_INTEGER:
      {
        #if PY_MAJOR_VERSION >= 3
        int64_t res = PyLong_AsLong(reso);
        #else
        int64_t res = PyInt_AsLong(reso);
        #endif
        self->file_->AllocateSignedIntegerField(nameStr, res, parent);
        break;
      }
      case XCDF_FLOATING_POINT:
      {
        double res = PyFloat_AsDouble(reso);
        self->file_->AllocateFloatingPointField(nameStr, res, parent);
        break;
      }
      default:
        Py_INCREF(Py_False);
        return Py_False;
    }
  }
  catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    return NULL;
  }

  Py_INCREF(Py_True);
  return Py_True;
}

// Method definitions for XCDFFile object
static PyMethodDef XCDFFile_methods[] =
{
  { const_cast<char*>("header"), (PyCFunction)XCDFFile_header,
    METH_NOARGS,
    const_cast<char*>("Print XCDF file field data") },

  { const_cast<char*>("getRecord"), (PyCFunction)XCDFFile_getRecord,
    METH_VARARGS,
    const_cast<char*>("Get a record by number from the file") },

  { const_cast<char*>("records"), (PyCFunction)XCDFRecord_iterator,
    METH_KEYWORDS,
    const_cast<char*>("Iterator over XCDF records") },

  { const_cast<char*>("fields"), (PyCFunction)XCDFField_iterator,
    METH_O,
    const_cast<char*>("Iterator over one or more XCDF fields (comma-separated "
                      "by name)") },

  { const_cast<char*>("addField"), (PyCFunction)XCDFFile_addField,
    METH_VARARGS,
    const_cast<char*>("Add a field with a given name, XCDF type, and optional "
                      "resolution") },

  { NULL }
};

// _____________________
// XCDFFile get/setters \_______________________________________________________
static PyObject*
XCDFFile_getfilename(pyxcdf_XCDFFile* self, void* closure)
{
  Py_INCREF(self->filename_);
  return self->filename_;
}

static PyObject*
XCDFFile_getcount(pyxcdf_XCDFFile* self, void* closure)
{
  try {
    PyObject* tmp = Py_BuildValue("l", self->file_->GetEventCount());
    return tmp;
  }
  catch (const XCDFException& e) {
    PyErr_SetString(pyxcdf_XCDFException, e.GetMessage().c_str());
    return NULL;
  }
}

static PyObject*
XCDFFile_getnfields(pyxcdf_XCDFFile* self, void* closure)
{
  PyObject* tmp = Py_BuildValue("l", self->file_->GetNFields());
  return tmp;
}

static PyGetSetDef XCDFFile_getseters[] =
{
  { const_cast<char*>("filename"), (getter)XCDFFile_getfilename, NULL,
    const_cast<char*>("XCDF file name"),
    NULL },

  { const_cast<char*>("count"), (getter)XCDFFile_getcount, NULL,
    const_cast<char*>("XCDF record count"),
    NULL },

  { const_cast<char*>("nfields"), (getter)XCDFFile_getnfields, NULL,
    const_cast<char*>("Number of fields per record"),
    NULL },

  { NULL }
};

// ________________________________
// XCDFFile type object definition \____________________________________________
static PyTypeObject
pyxcdf_XCDFFileType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyxcdf.XCDFFile",                        // tp_name
    sizeof(pyxcdf_XCDFFile),                  // tp_basicsize
    0,                                        // tp_itemsize
    (destructor)XCDFFile_dealloc,             // tp_dealloc
    0,                                        // tp_print
    0,                                        // tp_getattr
    0,                                        // tp_setattr
    0,                                        // tp_compare
    0,                                        // tp_repr
    0,                                        // tp_as_number
    0,                                        // tp_as_sequence
    0,                                        // tp_as_mapping
    0,                                        // tp_hash
    0,                                        // tp_call
    0,                                        // tp_str
    0,                                        // tp_getattro
    0,                                        // tp_setattro
    0,                                        // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, // tp_flags
    "XCDFFile object",                        // tp_doc
    0,                                        // tp_traverse
    0,                                        // tp_clear
    0,                                        // tp_richcompare
    0,                                        // tp_weaklistoffset
    0,                                        // tp_iter
    0,                                        // tp_iternext
    XCDFFile_methods,                         // tp_methods
    XCDFFile_members,                         // tp_members
    XCDFFile_getseters,                       // tp_getset
    0,                                        // tp_base
    0,                                        // tp_dict
    0,                                        // tp_descr_get
    0,                                        // tp_descr_set
    0,                                        // tp_dictoffset
    (initproc)XCDFFile_init,                  // tp_init
    0,                                        // tp_alloc
    XCDFFile_new,                             // tp_new
};

static PyMethodDef pyxcdf_methods[] =
{
  { NULL }
};

// _________________________
// Python module definition \___________________________________________________
#if PY_MAJOR_VERSION >= 3
  static struct PyModuleDef pyxcdfModule = {
    PyModuleDef_HEAD_INIT,
    "pyxcdf",                 // m_name
    NULL,                   // m_doc
    -1,                       // m_size
  };

  PyMODINIT_FUNC
  PyInit_pyxcdf(void)
  {
    PyObject* module = PyModule_Create(&pyxcdfModule);
    if (!module)
      return NULL;

    pyxcdf_XCDFException = PyErr_NewException(
           const_cast<char*>("pyxcdf.XCDFException"), NULL, NULL);
    Py_INCREF(pyxcdf_XCDFException);
    PyModule_AddObject(module, "XCDFException", pyxcdf_XCDFException);
  }
#else
  PyMODINIT_FUNC
  initpyxcdf(void)
  {
    // Enable creation of new XCDFFile objects
    if (PyType_Ready(&pyxcdf_XCDFFileType) < 0)
      return;

    if (PyType_Ready(&XCDFRecordIteratorType) < 0)
      return;

    if (PyType_Ready(&XCDFFieldIteratorType) < 0)
      return;

    PyObject* module = Py_InitModule3("pyxcdf", pyxcdf_methods,
                                      "Python bindings to XCDF library.");

    // Add XCDFFile type to the module dictionary
    Py_INCREF(&pyxcdf_XCDFFileType);
    PyModule_AddObject(module, "XCDFFile", (PyObject*)&pyxcdf_XCDFFileType);

    // Add XCDFException
    pyxcdf_XCDFException = PyErr_NewException(
               const_cast<char*>("pyxcdf.XCDFException"), NULL, NULL);
    Py_INCREF(pyxcdf_XCDFException);
    PyModule_AddObject(module, "XCDFException", pyxcdf_XCDFException);

    // Add XCDF enum types to the module
    PyModule_AddIntConstant(module, "XCDF_SIGNED_INTEGER",
                                    int(XCDF_SIGNED_INTEGER));
    PyModule_AddIntConstant(module, "XCDF_UNSIGNED_INTEGER",
                                    int(XCDF_UNSIGNED_INTEGER));
    PyModule_AddIntConstant(module, "XCDF_FLOATING_POINT",
                                    int(XCDF_FLOATING_POINT));
  }
#endif

