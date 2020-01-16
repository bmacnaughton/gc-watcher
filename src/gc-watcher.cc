
#include <napi.h>
#include <v8.h>
#include <uv.h>
#include <iostream>

//
// non-class version
//
Napi::FunctionReference cbBefore;
Napi::FunctionReference cbAfter;

// forward
void beforeGC(v8::Isolate*, v8::GCType, v8::GCCallbackFlags);
void afterGC(v8::Isolate*, v8::GCType, v8::GCCallbackFlags);

// functions that register/deregister internal handlers
static inline void AddGCPrologueCallback(
    v8::Isolate::GCCallback callback,
    v8::GCType gc_type_filter = v8::kGCTypeAll) {
  v8::Isolate::GetCurrent()->AddGCPrologueCallback(callback, gc_type_filter);
}
static inline void RemoveGCPrologueCallback(
    v8::Isolate::GCCallback callback) {
  v8::Isolate::GetCurrent()->RemoveGCPrologueCallback(callback);
}
static inline void AddGCEpilogueCallback(
    v8::Isolate::GCCallback callback,
    v8::GCType gc_type_filter = v8::kGCTypeAll) {
  v8::Isolate::GetCurrent()->AddGCEpilogueCallback(callback, gc_type_filter);
}
static inline void RemoveGCEpilogueCallback(v8::Isolate::GCCallback callback) {
  v8::Isolate::GetCurrent()->RemoveGCEpilogueCallback(callback);
}

//
// enable and disable v8's callbacks
//
bool gc_callbacks_enabled = false;

static int enable_gc_callbacks() {
  if (gc_callbacks_enabled) {
    return false;
  }
  AddGCPrologueCallback(beforeGC);
  AddGCEpilogueCallback(afterGC);
  gc_callbacks_enabled = true;
  return true;
}

static int disable_gc_callbacks() {
  if (!gc_callbacks_enabled) {
    return false;
  }
  RemoveGCPrologueCallback(beforeGC);
  RemoveGCEpilogueCallback(afterGC);
  gc_callbacks_enabled = false;
  return true;
}

//
// callbacks for v8
//
bool callbacks_present = false;

typedef struct raw {
} raw_t;

// two time windows - per callback and cumulative
typedef struct raw_counts {
  uint64_t gc_time;
  uint64_t gc_count;
  uint64_t out_of_order_count;
} raw_counts_t;

raw_counts_t raw;
raw_counts_t begin;             // at beforeGC callback
raw_counts_t cumulative;


Napi::Value enable(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() != 0 && (info.Length() != 2 || !info[0].IsFunction() || !info[1].IsFunction())) {
    Napi::TypeError::New(env, "invalid signature").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // so much for encapsulation of state...
  if (gc_callbacks_enabled) {
    Napi::Error::New(env, "already enabled").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  raw.gc_time = 0;
  raw.gc_count = 0;
  raw.out_of_order_count = 0;
  begin.gc_time = 0;
  begin.gc_count = 0;
  begin.out_of_order_count = 0;
  cumulative.gc_time = 0;
  cumulative.gc_count = 0;
  cumulative.out_of_order_count = 0;

  // save javascript callbacks if present
  if (info.Length() == 2) {
    callbacks_present = true;
    cbBefore = Napi::Persistent(info[0].As<Napi::Function>());
    cbBefore.SuppressDestruct();
    cbAfter = Napi::Persistent(info[1].As<Napi::Function>());
    cbAfter.SuppressDestruct();
  }

  // start listening to v8 callbacks.
  enable_gc_callbacks();

  return Napi::Number::New(env, 1);
}

Napi::Value disable(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (gc_callbacks_enabled) {
    disable_gc_callbacks();
  }
  // don't know how to do this using node-addon-api.
  napi_delete_reference(cbBefore.Env(), cbBefore);
  napi_delete_reference(cbAfter.Env(), cbAfter);

  return Napi::Number::New(env, 1);
}

//
// start collecting data
//
Napi::Value get_cumulative(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  Napi::Object options;
  if (info[0].IsObject()) {
    options = info[0].ToObject();
  }

  Napi::Value gcTime = Napi::Number::New(env, cumulative.gc_time);
  Napi::Value gcCount = Napi::Number::New(env, cumulative.gc_count);
  Napi::Value errors = Napi::Number::New(env, cumulative.out_of_order_count);

  // presume reset is the typical case so make it the default. don't
  // reset only if {reset: falsey} is specified.
  bool reset = true;
  if (options != nullptr && options.Has("reset")) {
    reset = options.Get("reset").As<Napi::Boolean>().ToBoolean();
  }
  if (reset) {
    cumulative.gc_time = 0;
    cumulative.gc_count = 0;
    cumulative.out_of_order_count = 0;
  }

  Napi::Object o = Napi::Object::New(env);
  o.Set("gcTime", gcTime);
  o.Set("gcCount", gcCount);
  o.Set("errors", errors);

  return o;
}


//
// callbacks when gc events take place
//
uint64_t start_time = 0;
bool was_before = false;

void beforeGC(v8::Isolate* isolate,
              v8::GCType type,
              v8::GCCallbackFlags flags) {
  //std::cout << "before:" << type << ":" << std::hex << flags << std::endl;

  // save the starting values
  start_time = uv_hrtime();
  begin = raw;

  if (cbBefore != nullptr) {
    Napi::Env env = cbBefore.Env();
    Napi::Value jstype = Napi::Number::New(env, type);
    Napi::Value jsflags = Napi::Number::New(env, flags);
    Napi::Value wasBefore = Napi::Boolean::New(env, was_before);
    std::vector<napi_value> args = {jstype, jsflags, wasBefore};

    cbBefore.Call(args);
    if (env.IsExceptionPending()) {
      Napi::Error e = env.GetAndClearPendingException();
      std::cerr << "before exception:" + e.Message() + "\n";
      // e.ThrowAsJavaScriptException();
    }
  }
  // did we get two beforeGC callbacks in a row?
  if (was_before) {
    raw.out_of_order_count += 1;
  }
  was_before = true;
}
void afterGC(v8::Isolate *isolate,
             v8::GCType type,
             v8::GCCallbackFlags flags) {
  //std::cout << "after:" << type << ":" << std::hex << flags << std:: endl;

  uint64_t delta_time = uv_hrtime() - start_time;
  raw.gc_time += delta_time;
  raw.gc_count += 1;

  if (!was_before) {
    raw.out_of_order_count += 1;
  }
  was_before = false;

  // accumulate times across many callbacks.
  cumulative.gc_time += raw.gc_time - begin.gc_time;
  cumulative.gc_count += raw.gc_count - begin.gc_count;
  cumulative.out_of_order_count += raw.out_of_order_count - begin.out_of_order_count;

  if (cbAfter != nullptr) {
    Napi::Env env = cbAfter.Env();
    Napi::Value jstype = Napi::Number::New(env, type);
    Napi::Value jsflags = Napi::Number::New(env, flags);
    Napi::Value deltaTime = Napi::Number::New(env, delta_time);
    Napi::Value gcCount = Napi::Number::New(env, 1);
    Napi::Value error = Napi::Boolean::New(env, begin.out_of_order_count != raw.out_of_order_count);
    std::vector<napi_value> args = {jstype, jsflags, deltaTime, gcCount, error};

    cbAfter.Call(args);
    if (env.IsExceptionPending()) {
      Napi::Error e = env.GetAndClearPendingException();
      std::cerr << "after exception:" + e.Message() + "\n";
      //e.ThrowAsJavaScriptException();
    }
  }
}



Napi::Object Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  exports.Set("enable", Napi::Function::New(env, enable));
  exports.Set("disable", Napi::Function::New(env, disable));
  exports.Set("getCumulative", Napi::Function::New(env, get_cumulative));

  return exports;
}

NODE_API_MODULE(hello, Init)
