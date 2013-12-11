// Copyright (c) 2013 GitHub, Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/node_bindings.h"

#include <limits.h>  // PATH_MAX

#include "base/command_line.h"
#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/base_paths.h"
#include "base/path_service.h"
#include "common/v8/native_type_conversions.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/escape.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "vendor/node/src/node_javascript.h"

#if defined(OS_WIN)
#include "base/strings/utf_string_conversions.h"
#endif

#include "common/v8/node_common.h"

using content::BrowserThread;

// Forward declaration of internal node functions.
namespace node {
void Init(int* argc,
          const char** argv,
          int* exec_argc,
          const char*** exec_argv);
Environment* CreateEnvironment(v8::Isolate* isolate,
                               int argc,
                               const char* const* argv,
                               int exec_argc,
                               const char* const* exec_argv);
}

namespace atom {

namespace {

void UvNoOp(uv_async_t* handle, int status) {
}

}  // namespace

node::Environment* global_env = NULL;

NodeBindings::NodeBindings(bool is_browser)
    : is_browser_(is_browser),
      message_loop_(NULL),
      uv_loop_(uv_default_loop()),
      embed_closed_(false) {
}

NodeBindings::~NodeBindings() {
  // Quit the embed thread.
  embed_closed_ = true;
  uv_sem_post(&embed_sem_);
  WakeupEmbedThread();

  // Wait for everything to be done.
  uv_thread_join(&embed_thread_);
  message_loop_->RunUntilIdle();

  // Clear uv.
  uv_sem_destroy(&embed_sem_);
  uv_timer_stop(&idle_timer_);
}

void NodeBindings::Initialize() {
  CommandLine::StringVector str_argv = CommandLine::ForCurrentProcess()->argv();

#if defined(OS_WIN)
  std::vector<std::string> utf8_str_argv;
  utf8_str_argv.reserve(str_argv.size());
#endif

  // Convert string vector to const char* array.
  std::vector<const char*> args(str_argv.size(), NULL);
  for (size_t i = 0; i < str_argv.size(); ++i) {
#if defined(OS_WIN)
    utf8_str_argv.push_back(UTF16ToUTF8(str_argv[i]));
    args[i] = utf8_str_argv[i].c_str();
#else
    args[i] = str_argv[i].c_str();
#endif
  }

  // Feed node the path to initialization script.
  base::FilePath exec_path(str_argv[0]);
  PathService::Get(base::FILE_EXE, &exec_path);
  base::FilePath resources_path =
#if defined(OS_MACOSX)
      is_browser_ ? exec_path.DirName().DirName().Append("Resources") :
                    exec_path.DirName().DirName().DirName().DirName().DirName()
                             .Append("Resources");
#else
      exec_path.DirName().Append("resources");
#endif
  base::FilePath script_path =
      resources_path.AppendASCII("browser")
                    .AppendASCII("atom")
                    .AppendASCII(is_browser_ ? "atom.js" : "atom-renderer.js");
  std::string script_path_str = script_path.AsUTF8Unsafe();
  args.insert(args.begin() + 1, script_path_str.c_str());

  // Init idle GC for browser.
  if (is_browser_) {
    uv_timer_init(uv_default_loop(), &idle_timer_);
    uv_timer_start(&idle_timer_, IdleCallback, 5000, 5000);
  }

  // Open node's error reporting system for browser process.
  node::g_standalone_mode = is_browser_;
  node::g_upstream_node_mode = false;

  // Init node.
  int argc = args.size();
  const char** argv = &args[0];
  int exec_argc;
  const char** exec_argv;
  node::Init(&argc, argv, &exec_argc, &exec_argv);
  v8::V8::Initialize();

  // Create environment (setup process object and load node.js).
  global_env = node::CreateEnvironment(node_isolate, argc, argv, argc, argv);
}

void NodeBindings::BindTo(WebKit::WebFrame* frame) {
  v8::HandleScope handle_scope(node_isolate);

  v8::Handle<v8::Context> context = frame->mainWorldScriptContext();
  if (context.IsEmpty())
    return;

  v8::Context::Scope scope(context);

  // Erase security token.
  context->SetSecurityToken(global_env->context()->GetSecurityToken());

  // Evaluate cefode.js.
  v8::Handle<v8::Script> script = node::CompileCefodeMainSource();
  v8::Local<v8::Value> result = script->Run();

  // Run the script of cefode.js.
  std::string script_path(GURL(frame->document().url()).path());
  script_path = net::UnescapeURLComponent(
      script_path,
      net::UnescapeRule::SPACES | net::UnescapeRule::URL_SPECIAL_CHARS);
  v8::Handle<v8::Value> args[2] = {
    global_env->process_object(),
    ToV8Value(script_path),
  };
  v8::Local<v8::Function>::Cast(result)->Call(context->Global(), 2, args);
}

void NodeBindings::PrepareMessageLoop() {
  DCHECK(!is_browser_ || BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Add dummy handle for libuv, otherwise libuv would quit when there is
  // nothing to do.
  uv_async_init(uv_loop_, &dummy_uv_handle_, UvNoOp);

  // Start worker that will interrupt main loop when having uv events.
  uv_sem_init(&embed_sem_, 0);
  uv_thread_create(&embed_thread_, EmbedThreadRunner, this);
}

void NodeBindings::RunMessageLoop() {
  DCHECK(!is_browser_ || BrowserThread::CurrentlyOn(BrowserThread::UI));

  // The MessageLoop should have been created, remember the one in main thread.
  message_loop_ = base::MessageLoop::current();

  // Run uv loop for once to give the uv__io_poll a chance to add all events.
  UvRunOnce();
}

void NodeBindings::UvRunOnce() {
  DCHECK(!is_browser_ || BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Enter node context while dealing with uv events.
  v8::HandleScope handle_scope(node_isolate);
  v8::Context::Scope context_scope(global_env->context());

  // Deal with uv events.
  int r = uv_run(uv_loop_, (uv_run_mode)(UV_RUN_ONCE | UV_RUN_NOWAIT));
  if (r == 0 || uv_loop_->stop_flag != 0)
    message_loop_->QuitWhenIdle();  // Quit from uv.

  // Tell the worker thread to continue polling.
  uv_sem_post(&embed_sem_);
}

void NodeBindings::WakeupMainThread() {
  DCHECK(message_loop_);
  message_loop_->PostTask(FROM_HERE, base::Bind(&NodeBindings::UvRunOnce,
                                                base::Unretained(this)));
}

void NodeBindings::WakeupEmbedThread() {
  uv_async_send(&dummy_uv_handle_);
}

// static
void NodeBindings::EmbedThreadRunner(void *arg) {
  NodeBindings* self = static_cast<NodeBindings*>(arg);

  while (!self->embed_closed_) {
    // Wait for the main loop to deal with events.
    uv_sem_wait(&self->embed_sem_);

    self->PollEvents();

    // Deal with event in main thread.
    self->WakeupMainThread();
  }
}

// static
void NodeBindings::IdleCallback(uv_timer_t*, int) {
  v8::V8::IdleNotification();
}

}  // namespace atom
