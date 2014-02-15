#include "duktapevm.h"

#include <node.h>
#include <v8.h>

#include <iostream>
#include <string>

#include <map>
#include <functional>
#include <memory>

using namespace v8;
using node::FatalException;

namespace {

struct WorkRequest
{
	WorkRequest(std::string functionName, std::string parameters, std::string script, Persistent<Function> callback):
	 functionName(std::move(functionName))
	,parameters(std::move(parameters))
	,script(std::move(script))
	,callback(callback)
	,hasError(false)
	{		
	};

	duktape::DuktapeVM vm;

	// in
	std::string functionName;
	std::string parameters;
	std::string script;

	// out
	Persistent<Function> callback;
	bool hasError;
	std::string returnValue;
};

struct ScopedUvWorkRequest
{
	ScopedUvWorkRequest(uv_work_t* work):
	 m_work(work)
	,m_workRequest(static_cast<WorkRequest*> (m_work->data))
	{
	}

	~ScopedUvWorkRequest()
	{
		m_workRequest->callback.Dispose();
		delete m_workRequest;
		delete m_work;
	}

	WorkRequest* getWorkRequest()
	{
		return m_workRequest;
	}

private:
	uv_work_t* m_work;
	WorkRequest* m_workRequest;
};

struct APICallbackSignaling
{	
	APICallbackSignaling(Persistent<Function> callback, std::string parameter):
	 callback(callback)
	,parameter(parameter)
	,returnValue("")
	{}

	Persistent<Function> callback;
	std::string parameter;
	std::string returnValue;
};

uv_async_t async;
uv_cond_t cv;
uv_mutex_t mutex;

void onWork(uv_work_t* req)
{
	// Do not use scoped-wrapper as req is still needed in onWorkDone.
	WorkRequest* work = static_cast<WorkRequest*> (req->data);

	auto ret = work->vm.run(work->functionName, work->parameters, work->script);
	work->hasError = ret.errorCode != 0;
	work->returnValue = ret.value;
}

void onWorkDone(uv_work_t* req, int status)
{
	// Dispose thread signaling.
	uv_close((uv_handle_t*) &async, NULL);
	uv_mutex_destroy(&mutex);
	uv_cond_destroy(&cv);

	ScopedUvWorkRequest uvReq(req);
	WorkRequest* work = uvReq.getWorkRequest();

	HandleScope scope;

	Handle<Value> argv[2];
	argv[0] = Boolean::New(work->hasError);
	argv[1] = String::New(work->returnValue.c_str());

	TryCatch try_catch;
	work->callback->Call(Context::GetCurrent()->Global(), 2, argv);

	if (try_catch.HasCaught()) 
	{
		FatalException(try_catch);
	}

}

void callV8FunctionOnMainThread(uv_async_t *handle, int status) 
{
	uv_mutex_lock(&mutex);
	auto signalData = static_cast<APICallbackSignaling*> (handle->data);

	HandleScope scope;
	Handle<Value> argv[1];
	argv[0] = String::New(signalData->parameter.c_str());
	auto retVal = signalData->callback->Call(Context::GetCurrent()->Global(), 1, argv);
	String::Utf8Value retString(retVal);
	signalData->returnValue = std::string(*retString);

	uv_cond_signal(&cv);
	uv_mutex_unlock(&mutex);
}

Handle<Value> run(const Arguments& args) 
{
	HandleScope scope;
	if(args.Length() < 5) 
	{
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}

	if (!args[0]->IsString() || !args[1]->IsString() || !args[2]->IsString() || !args[4]->IsFunction()) 
	{
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}

	String::Utf8Value functionName(args[0]->ToString());
	String::Utf8Value parameters(args[1]->ToString());
	String::Utf8Value script(args[2]->ToString());
	Local<Function> returnCallback = Local<Function>::Cast(args[4]);

	WorkRequest* workReq = new WorkRequest(	std::string(*functionName), 
											std::string(*parameters), 
											std::string(*script), 
											Persistent<Function>::New(returnCallback));

	// API
	if(args[3]->IsObject())
	{
		auto object = Handle<Object>::Cast(args[3]);
		auto properties = object->GetPropertyNames();

		auto len = properties->Length();
		for(unsigned int i = 0; i < len; ++i)
		{
			Local<Value> key = properties->Get(i);
			Local<Value> value = object->Get(key);
			if(!key->IsString() || !value->IsFunction())
			{
				ThrowException(Exception::Error(String::New("Error in API-definition")));
				return scope.Close(Undefined());
			}


			auto apiCallbackFunc = Local<Function>::Cast(value);
			auto persistentApiCallbackFunc = Persistent<Function>::New(apiCallbackFunc);
			auto duktapeToNodeBridge = duktape::Callback([persistentApiCallbackFunc] (std::string parameter) 
			{
				// We're on not on libuv/V8 main thread. Signal main to run 
				// callback function and wait for an answer.
				uv_mutex_lock(&mutex);

				std::unique_ptr<APICallbackSignaling> cbsignaling(new APICallbackSignaling(persistentApiCallbackFunc, parameter));
				async.data = (void*) cbsignaling.get();
		        uv_async_send(&async);

		        uv_cond_wait(&cv, &mutex);
		        uv_mutex_unlock(&mutex);

		        std::string retStr(cbsignaling->returnValue);

				return retStr;
			});

			String::Utf8Value keyStr(key);
			workReq->vm.registerCallback(std::string(*keyStr), duktapeToNodeBridge);
		}		
	}


    uv_work_t* req = new uv_work_t();
    req->data = workReq;

	uv_mutex_init(&mutex);
	uv_cond_init(&cv);
	uv_async_init(uv_default_loop(), &async, callV8FunctionOnMainThread);
	uv_queue_work(uv_default_loop(), req, onWork, onWorkDone);

	return scope.Close(Undefined());
}

Handle<Value> runSync(const Arguments& args) 
{
	HandleScope scope;
	if(args.Length() < 3) 
	{
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}

	if (!args[0]->IsString() || !args[1]->IsString() || !args[2]->IsString()) 
	{
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}

	duktape::DuktapeVM vm;

	if(args[3]->IsObject())
	{
		auto object = Handle<Object>::Cast(args[3]);
		auto properties = object->GetPropertyNames();

		const auto len = properties->Length();
		for(unsigned int i = 0; i < len; ++i)
		{
			const Local<Value> key = properties->Get(i);
			const Local<Value> value = object->Get(key);
			if(!key->IsString() || !value->IsFunction())
			{
				ThrowException(Exception::Error(String::New("Error in API-definition")));
				return scope.Close(Undefined());
			}

			Local<Function> apiCallbackFunc = Local<Function>::Cast(value);

			auto duktapeToNodeBridge = duktape::Callback([apiCallbackFunc] (std::string paramString) 
			{
				Handle<Value> argv[1];
				argv[0] = String::New(paramString.c_str());

				auto retVal = apiCallbackFunc->Call(Context::GetCurrent()->Global(), 1, argv);

				String::Utf8Value retString(retVal);

				return std::string(*retString);
			});

			String::Utf8Value keyStr(key);
			vm.registerCallback(std::string(*keyStr), duktapeToNodeBridge);
		}		
	}

 	String::Utf8Value functionName(args[0]->ToString());
	String::Utf8Value parameters(args[1]->ToString());
	String::Utf8Value script(args[2]->ToString());

	auto ret = vm.run(std::string(*functionName), std::string(*parameters), std::string(*script));

	if(ret.errorCode != 0)
	{
		ThrowException(Exception::Error(String::New(ret.value.c_str())));
		return scope.Close(Undefined());
	}

	return scope.Close(String::New(ret.value.c_str()));
}

void init(Handle<Object> exports) 
{
	exports->Set(String::NewSymbol("runSync"), FunctionTemplate::New(runSync)->GetFunction());
	exports->Set(String::NewSymbol("run"), FunctionTemplate::New(run)->GetFunction());
}

} // unnamed namespace

NODE_MODULE(duktape, init)
