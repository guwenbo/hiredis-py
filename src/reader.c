#include "reader.h"

#include <assert.h>

static void Reader_dealloc(hiredis_ReaderObject *self);
static int Reader_init(hiredis_ReaderObject *self, PyObject *args, PyObject *kwds);
static PyObject *Reader_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static PyObject *Reader_feed(hiredis_ReaderObject *self, PyObject *args);
static PyObject *Reader_gets(hiredis_ReaderObject *self, PyObject *args);
static PyObject *Reader_setmaxbuf(hiredis_ReaderObject *self, PyObject *arg);
static PyObject *Reader_getmaxbuf(hiredis_ReaderObject *self);
static PyObject *Reader_len(hiredis_ReaderObject *self);
static PyObject *Reader_has_data(hiredis_ReaderObject *self);

static PyMethodDef hiredis_ReaderMethods[] = {
    {"feed", (PyCFunction)Reader_feed, METH_VARARGS, NULL },
    {"gets", (PyCFunction)Reader_gets, METH_VARARGS, NULL },
    {"setmaxbuf", (PyCFunction)Reader_setmaxbuf, METH_O, NULL },
    {"getmaxbuf", (PyCFunction)Reader_getmaxbuf, METH_NOARGS, NULL },
    {"len", (PyCFunction)Reader_len, METH_NOARGS, NULL },
    {"has_data", (PyCFunction)Reader_has_data, METH_NOARGS, NULL },
    { NULL }  /* Sentinel */
};

PyTypeObject hiredis_ReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    MOD_HIREDIS ".Reader",        /*tp_name*/
    sizeof(hiredis_ReaderObject), /*tp_basicsize*/
    0,                            /*tp_itemsize*/
    (destructor)Reader_dealloc,   /*tp_dealloc*/
    0,                            /*tp_print*/
    0,                            /*tp_getattr*/
    0,                            /*tp_setattr*/
    0,                            /*tp_compare*/
    0,                            /*tp_repr*/
    0,                            /*tp_as_number*/
    0,                            /*tp_as_sequence*/
    0,                            /*tp_as_mapping*/
    0,                            /*tp_hash */
    0,                            /*tp_call*/
    0,                            /*tp_str*/
    0,                            /*tp_getattro*/
    0,                            /*tp_setattro*/
    0,                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Hiredis protocol reader",    /*tp_doc */
    0,                            /*tp_traverse */
    0,                            /*tp_clear */
    0,                            /*tp_richcompare */
    0,                            /*tp_weaklistoffset */
    0,                            /*tp_iter */
    0,                            /*tp_iternext */
    hiredis_ReaderMethods,        /*tp_methods */
    0,                            /*tp_members */
    0,                            /*tp_getset */
    0,                            /*tp_base */
    0,                            /*tp_dict */
    0,                            /*tp_descr_get */
    0,                            /*tp_descr_set */
    0,                            /*tp_dictoffset */
    (initproc)Reader_init,        /*tp_init */
    0,                            /*tp_alloc */
    Reader_new,                   /*tp_new */
};

static void *tryParentize(const redisReadTask *task, PyObject *obj) {
    PyObject *parent;
    if (task && task->parent) {
        parent = (PyObject*)task->parent->obj;
        assert(PyList_CheckExact(parent));
        PyList_SET_ITEM(parent, task->idx, obj);
    }
    return obj;
}

static PyObject *createDecodedString(hiredis_ReaderObject *self, const char *str, size_t len) {
    PyObject *obj;

    if (self->encoding == NULL || !self->shouldDecode) {
        obj = PyBytes_FromStringAndSize(str, len);
    } else {
        obj = PyUnicode_Decode(str, len, self->encoding, self->errors);
        if (obj == NULL) {
            /* Store error when this is the first. */
            if (self->error.ptype == NULL)
                PyErr_Fetch(&(self->error.ptype), &(self->error.pvalue),
                        &(self->error.ptraceback));

            /* Return Py_None as placeholder to let the error bubble up and
             * be used when a full reply in Reader#gets(). */
            obj = Py_None;
            Py_INCREF(obj);
            PyErr_Clear();
        }
    }

    assert(obj != NULL);
    return obj;
}

static void *createError(PyObject *errorCallable, char *errstr, size_t len) {
    PyObject *obj, *errmsg;

    #if IS_PY3K
    errmsg = PyUnicode_DecodeUTF8(errstr, len, "replace");
    #else
    errmsg = Py_BuildValue("s#", errstr, len);
    #endif
    assert(errmsg != NULL); /* TODO: properly handle OOM etc */

    obj = PyObject_CallFunctionObjArgs(errorCallable, errmsg, NULL);
    Py_DECREF(errmsg);
    /* obj can be NULL if custom error class raised another exception */

    return obj;
}

static void *createStringObject(const redisReadTask *task, char *str, size_t len) {
    hiredis_ReaderObject *self = (hiredis_ReaderObject*)task->privdata;
    PyObject *obj;

    if (task->type == REDIS_REPLY_ERROR) {
        obj = createError(self->replyErrorClass, str, len);
        if (obj == NULL) {
            if (self->error.ptype == NULL)
                PyErr_Fetch(&(self->error.ptype), &(self->error.pvalue),
                        &(self->error.ptraceback));
            obj = Py_None;
            Py_INCREF(obj);
        }
    } else {
        obj = createDecodedString(self, str, len);
    }
    return tryParentize(task, obj);
}

static void *createArrayObject(const redisReadTask *task, int elements) {
    PyObject *obj;
    obj = PyList_New(elements);
    return tryParentize(task, obj);
}

static void *createIntegerObject(const redisReadTask *task, long long value) {
    PyObject *obj;
    obj = PyLong_FromLongLong(value);
    return tryParentize(task, obj);
}

static void *createNilObject(const redisReadTask *task) {
    PyObject *obj = Py_None;
    Py_INCREF(obj);
    return tryParentize(task, obj);
}

static void freeObject(void *obj) {
    Py_XDECREF(obj);
}

redisReplyObjectFunctions hiredis_ObjectFunctions = {
    createStringObject,  // void *(*createString)(const redisReadTask*, char*, size_t);
    createArrayObject,   // void *(*createArray)(const redisReadTask*, int);
    createIntegerObject, // void *(*createInteger)(const redisReadTask*, long long);
    createNilObject,     // void *(*createNil)(const redisReadTask*);
    freeObject           // void (*freeObject)(void*);
};

static void Reader_dealloc(hiredis_ReaderObject *self) {
    // we don't need to free self->encoding as the buffer is managed by Python
    // https://docs.python.org/3/c-api/arg.html#strings-and-buffers
    redisReplyReaderFree(self->reader);
    Py_XDECREF(self->protocolErrorClass);
    Py_XDECREF(self->replyErrorClass);

    ((PyObject *)self)->ob_type->tp_free((PyObject*)self);
}

static int _Reader_set_exception(PyObject **target, PyObject *value) {
    int callable;
    callable = PyCallable_Check(value);

    if (callable == 0) {
        PyErr_SetString(PyExc_TypeError, "Expected a callable");
        return 0;
    }

    Py_DECREF(*target);
    *target = value;
    Py_INCREF(*target);
    return 1;
}

static int Reader_init(hiredis_ReaderObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "protocolError", "replyError", "encoding", "errors", NULL };
    PyObject *protocolErrorClass = NULL;
    PyObject *replyErrorClass = NULL;
    char *encoding = NULL;
    char *errors = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOss", kwlist,
        &protocolErrorClass, &replyErrorClass, &encoding, &errors))
            return -1;

    if (protocolErrorClass)
        if (!_Reader_set_exception(&self->protocolErrorClass, protocolErrorClass))
            return -1;

    if (replyErrorClass)
        if (!_Reader_set_exception(&self->replyErrorClass, replyErrorClass))
            return -1;

    self->encoding = encoding;
    if (errors) {   // validate that the error handler exists, raises LookupError if not
        PyObject *codecs, *result;
        codecs = PyImport_ImportModule("codecs");
        if (!codecs)
            return -1;
        result = PyObject_CallMethod(codecs, "lookup_error", "s", errors);
        Py_DECREF(codecs);
        if (!result)
            return -1;
        Py_DECREF(result);
        self->errors = errors;
    }

    return 0;
}

static PyObject *Reader_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    hiredis_ReaderObject *self;
    self = (hiredis_ReaderObject*)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->reader = redisReaderCreateWithFunctions(NULL);
        self->reader->fn = &hiredis_ObjectFunctions;
        self->reader->privdata = self;

        self->encoding = NULL;
        self->errors = "strict";  // default to "strict" to mimic Python
        self->shouldDecode = 1;
        self->protocolErrorClass = HIREDIS_STATE->HiErr_ProtocolError;
        self->replyErrorClass = HIREDIS_STATE->HiErr_ReplyError;
        Py_INCREF(self->protocolErrorClass);
        Py_INCREF(self->replyErrorClass);

        self->error.ptype = NULL;
        self->error.pvalue = NULL;
        self->error.ptraceback = NULL;
    }
    return (PyObject*)self;
}

static PyObject *Reader_feed(hiredis_ReaderObject *self, PyObject *args) {
    Py_buffer buf;
    Py_ssize_t off = 0;
    Py_ssize_t len = -1;

    if (!PyArg_ParseTuple(args, "s*|nn", &buf, &off, &len)) {
        return NULL;
    }

    if (len == -1) {
      len = buf.len - off;
    }

    if (off < 0 || len < 0) {
      PyErr_SetString(PyExc_ValueError, "negative input");
      goto error;
    }

    if ((off + len) > buf.len) {
      PyErr_SetString(PyExc_ValueError, "input is larger than buffer size");
      goto error;
    }

    redisReplyReaderFeed(self->reader, (char *)buf.buf + off, len);
    PyBuffer_Release(&buf);
    Py_RETURN_NONE;

error:
    PyBuffer_Release(&buf);
    return NULL;
}

static PyObject *Reader_gets(hiredis_ReaderObject *self, PyObject *args) {
    PyObject *obj;
    PyObject *err;
    char *errstr;

    self->shouldDecode = 1;
    if (!PyArg_ParseTuple(args, "|i", &self->shouldDecode)) {
        return NULL;
    }

    if (redisReplyReaderGetReply(self->reader, (void**)&obj) == REDIS_ERR) {
        errstr = redisReplyReaderGetError(self->reader);
        /* protocolErrorClass might be a callable. call it, then use it's type */
        err = createError(self->protocolErrorClass, errstr, strlen(errstr));
        if (err != NULL) {
            obj = PyObject_Type(err);
            PyErr_SetString(obj, errstr);
            Py_DECREF(obj);
            Py_DECREF(err);
        }
        return NULL;
    }

    if (obj == NULL) {
        Py_RETURN_FALSE;
    } else {
        /* Restore error when there is one. */
        if (self->error.ptype != NULL) {
            Py_DECREF(obj);
            PyErr_Restore(self->error.ptype, self->error.pvalue,
                    self->error.ptraceback);
            self->error.ptype = NULL;
            self->error.pvalue = NULL;
            self->error.ptraceback = NULL;
            return NULL;
        }
        return obj;
    }
}

static PyObject *Reader_setmaxbuf(hiredis_ReaderObject *self, PyObject *arg) {
    long maxbuf;

    if (arg == Py_None)
        maxbuf = REDIS_READER_MAX_BUF;
    else {
        maxbuf = PyLong_AsLong(arg);
        if (maxbuf < 0) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_ValueError,
                                "maxbuf value out of range");
            return NULL;
        }
    }
    self->reader->maxbuf = maxbuf;

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *Reader_getmaxbuf(hiredis_ReaderObject *self) {
    return PyLong_FromSize_t(self->reader->maxbuf);
}

static PyObject *Reader_len(hiredis_ReaderObject *self) {
    return PyLong_FromSize_t(self->reader->len);
}

static PyObject *Reader_has_data(hiredis_ReaderObject *self) {
    if(self->reader->pos < self->reader->len)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}
