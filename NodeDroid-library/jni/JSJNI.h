//
// JSJNI.h
// AndroidJSCore project
//
// https://github.com/ericwlange/AndroidJSCore/
//
// Created by Eric Lange
//
/*
 Copyright (c) 2014-2016 Eric Lange. All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 - Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 - Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _JSJNI_H
#define _JSJNI_H

#include <stdlib.h>
#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <list>
#include <algorithm>
#include <set>
#include <thread>
#include <list>

#include "v8.h"
#include "libplatform/libplatform.h"
#include "uv.h"

using namespace v8;

#define NATIVE(package,rt,f) extern "C" JNIEXPORT \
    rt JNICALL Java_org_liquidplayer_javascript_##package##_##f
#define PARAMS JNIEnv* env, jobject thiz

//#define DEBUG_RETAINER 1

class Retainer {
public:
    Retainer() {
        m_count = 1;
#ifdef DEBUG_RETAINER
        m_debug_mutex.lock();
        m_debug.push_front(this);
        m_debug_mutex.unlock();
#endif
    }

    virtual int retain() {
        return ++m_count;
    }

    virtual int release() {
        if (--m_count == 0)
            delete this;
        return m_count;
    }

protected:
    virtual ~Retainer() {
#ifdef DEBUG_RETAINER
        m_debug_mutex.lock();
        m_debug.remove(this);
        m_debug_mutex.unlock();
#endif
    }

public:
    int m_count;

public:
#ifdef DEBUG_RETAINER
    static std::list<Retainer*> m_debug;
    static std::mutex m_debug_mutex;
#endif
};

class GenericAllocator : public ArrayBuffer::Allocator {
public:
    GenericAllocator() {}
    virtual ~GenericAllocator() {}
    virtual void* Allocate(size_t length) {
        unsigned char * mem =  (unsigned char *) malloc(length);
        memset(mem, 0, length);
        return (void*)mem;
    }
    virtual void* AllocateUninitialized(size_t length) {
        return malloc(length);
    }
    virtual void Free(void* data, size_t length) {
        free(data);
    }
};

struct Runnable {
    jobject thiz;
    jobject runnable;
    JavaVM *jvm;
};
class JSContext;

class ContextGroup : public Retainer {
public:
    ContextGroup();
    ContextGroup(Isolate *isolate, uv_loop_t *uv_loop);
    virtual Isolate* isolate() {
        return m_isolate;
    }
    virtual uv_loop_t * Loop() {
        return m_uv_loop;
    }
    virtual std::thread::id Thread() {
        return m_thread_id;
    }

    static void init_v8();
    static std::mutex *Mutex() { return &s_mutex; }
    static v8::Platform * Platform() { return s_platform; }
    static void callback(uv_async_t* handle);

protected:
    virtual ~ContextGroup();

private:
    static void dispose_v8();

    static v8::Platform *s_platform;
    static int s_init_count;
    static std::mutex s_mutex;

    Isolate *m_isolate;
    Isolate::CreateParams m_create_params;
    static GenericAllocator s_allocator;
    bool m_manage_isolate;
    uv_loop_t *m_uv_loop;
    std::thread::id m_thread_id;

public:
    uv_async_t *m_async_handle;
    std::list<struct Runnable *> m_runnables;
    std::mutex m_async_mutex;
};

template <typename T>
class JSValue;

// CRTP sucks
class JSContext : public Retainer {
public:
    JSContext(ContextGroup* isolate, Local<Context> val);
    virtual JSValue<Object> * Global();
    virtual Local<Context>    Value();
    virtual Isolate *         isolate();
    virtual ContextGroup *    Group();
    virtual void SetDefunct();
    virtual bool IsDefunct() { return m_isDefunct; }

    virtual void retain(JSValue<v8::Value>* value);
    virtual void release(JSValue<v8::Value>* value);
    virtual void retain(JSValue<v8::Object>* value);
    virtual void release(JSValue<v8::Object>* value);
    virtual void retain(JSValue<v8::Array>* value);
    virtual void release(JSValue<v8::Array>* value);
    virtual int retain() { return Retainer::retain(); }
    virtual int release() { return Retainer::release(); }

protected:
    virtual ~JSContext();

private:
    Persistent<Context, CopyablePersistentTraits<Context>> m_context;
    Persistent<Object, CopyablePersistentTraits<Object>> m_globalObject;
    ContextGroup *m_isolate;
    bool m_isDefunct;
    std::set<JSValue<v8::Value>*> m_value_set;
    std::set<JSValue<v8::Object>*> m_object_set;
    std::set<JSValue<v8::Array>*> m_array_set;
};

#define V8_ISOLATE(group,iso) \
        v8::Locker *lock_ = nullptr; \
        { \
            Isolate *iso = (group) ->isolate(); \
            ContextGroup* group_ = (group); \
            if (!(group)->Loop()) lock_ = new v8::Locker(iso); \
            Isolate::Scope isolate_scope_(iso); \
            HandleScope handle_scope_(iso);

#define V8_ISOLATE_CTX(ctx,iso,Ctx) \
        JSContext *context_ = reinterpret_cast<JSContext*>(ctx); \
        V8_ISOLATE(context_->Group(),iso); \
            Local<Context> Ctx = context_->Value(); \
            Context::Scope context_scope_(Ctx)

#define V8_UNLOCK() \
        } \
        if (lock_) delete lock_

template <typename T>
class JSValue : public Retainer {
public:
    virtual Local<T> Value() {
        if (m_isUndefined) {
            Local<v8::Value> undefined =
                Local<v8::Value>::New(isolate(),Undefined(isolate()));
            return *reinterpret_cast<Local<T> *>(&undefined);
        } else if (m_isNull) {
            Local<v8::Value> null =
                Local<v8::Value>::New(isolate(),Null(isolate()));
            return *reinterpret_cast<Local<T> *>(&null);
        } else {
            return Local<T>::New(isolate(), m_value);
        }
    }
    virtual Isolate* isolate() {
        return m_context->isolate();
    }
    virtual ContextGroup* Group() {
        return m_context->Group();
    }
    virtual JSContext* Context() { return m_context; }
    static JSValue<T> *New(JSContext* context, Local<T> val) {
        if (val->IsObject()) {
            Local<Private> privateKey = v8::Private::ForApi(context->isolate(),
                String::NewFromUtf8(context->isolate(), "__JSValue_ptr"));
            Local<Object> obj = val->ToObject(context->Value()).ToLocalChecked();
            Local<v8::Value> identifier;
            Maybe<bool> result = obj->HasPrivate(context->Value(), privateKey);
            bool hasPrivate = false;
            if (result.IsJust() && result.FromJust()) {
                hasPrivate = obj->GetPrivate(context->Value(), privateKey).ToLocal(&identifier);
            }
            if (hasPrivate && identifier->IsNumber()) {
                // This object is already wrapped, let's re-use it
                JSValue<T> *value =
                    reinterpret_cast<JSValue<T>*>(
                        (long)identifier->ToNumber(context->Value()).ToLocalChecked()->Value());
                value->retain();
                return value;
            } else {
                // First time wrap.  Create it new and mark it
                JSValue<T> *value = new JSValue<T>(context,val);
                obj->SetPrivate(context->Value(), privateKey,
                    Number::New(context->isolate(),(double)reinterpret_cast<long>(value)));
                return value;
            }
        } else {
            return new JSValue<T>(context,val);
        }
    }

protected:
    JSValue(JSContext* context, Local<T> val) {
        if (val->IsUndefined()) {
            m_isUndefined = true;
            m_isNull = false;
        } else if (val->IsNull()) {
            m_isUndefined = false;
            m_isNull = true;
        } else {
            m_value = Persistent<T,CopyablePersistentTraits<T>>(context->isolate(), val);
            m_isUndefined = false;
            m_isNull = false;
        }
        m_context = context;
        m_context->retain(this);
    }
    JSValue() {}
    virtual ~JSValue() {
        V8_ISOLATE(m_context->Group(), isolate);

        if (!m_isUndefined && !m_isNull) {
            if (Value()->IsObject()) {
                Local<Object> obj = Value()->ToObject(m_context->Value()).ToLocalChecked();
                // Clear wrapper pointer if it exists, in case this object is still held by JS
                Local<Private> privateKey = v8::Private::ForApi(isolate,
                    String::NewFromUtf8(isolate, "__JSValue_ptr"));
                obj->SetPrivate(m_context->Value(), privateKey,
                    Local<v8::Value>::New(isolate,Undefined(isolate)));
            }
            m_value.Reset();
        }

        m_context->release(this);
        V8_UNLOCK();
    }

protected:
    Persistent<T, CopyablePersistentTraits<T>> m_value;
    JSContext *m_context;
    bool m_isUndefined;
    bool m_isNull;

friend class JSContext;
};

#endif