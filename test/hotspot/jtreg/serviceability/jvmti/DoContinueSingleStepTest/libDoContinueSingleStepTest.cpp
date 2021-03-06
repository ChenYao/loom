/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <string.h>
#include "jvmti.h"

/*
 * The goal of this test is to single step into Continuation.doContinue(). The
 * expectation is that we will end up in Continuation.yield0() right after the
 * return from Continuation.doYield(). There have been bugs where yield0() was
 * compiled and we didn't get a single step event when resuming execution in it.
 * After confirming the yield0() single stepping, we turn off single stepping
 * and run to completion. There have been jvmti _cur_stack_depth asserts related
 * to doing this, although they were never reproduced with this test.
 *
 * Setting up a single step into Continuation.doContinue() is a bit tricky. It
 * is called from Continuation.run(), so the first step is to setup a breakpoint
 * at the start of run(). After it is hit, we setup another breakpoint at the
 * start of Continuation.isStarted(), which is called just before the doContinue()
 * call. Once it is hit, we enable single stepping. From isStarted() it should only
 * take about 14 single step to reach Continuation.yield0(). If we don't reach it by
 * 50 steps, the test fails.
 *
 * There's also a NotifyFramePop that is done. The is related to trying to trigger
 * the _cur_stack_depth assert.
 */
extern "C" {

#define MAX_FRAME_COUNT 20

static jvmtiEnv *jvmti = NULL;
static jthread exp_thread = NULL;
static jrawMonitorID event_mon = NULL;
static int breakpoint_count = 0;
static int single_step_count = 0;
static int method_entry_count = 0;
static int method_exit_count = 0;
static int frame_pop_count = 0;
static jboolean passed = JNI_FALSE;

static jmethodID *java_lang_Continuation_methods = NULL;
jint java_lang_Continuation_method_count = 0;
jclass java_lang_Continuation_class = NULL;

static void
lock_events() {
  jvmti->RawMonitorEnter(event_mon);
}

static void
unlock_events() {
  jvmti->RawMonitorExit(event_mon);
}

static void
check_jvmti_status(JNIEnv* jni, jvmtiError err, const char* msg) {
  if (err != JVMTI_ERROR_NONE) {
    printf("check_jvmti_status: JVMTI function returned error: %d\n", err);
    jni->FatalError(msg);
  }
}

static char* get_method_class_name(jvmtiEnv *jvmti, JNIEnv* jni, jmethodID method) {
  jvmtiError err;
  jclass klass = NULL;
  char*  cname = NULL;

  err = jvmti->GetMethodDeclaringClass(method, &klass);
  check_jvmti_status(jni, err, "get_method_class_name: error in JVMTI GetMethodDeclaringClass");

  err = jvmti->GetClassSignature(klass, &cname, NULL);
  check_jvmti_status(jni, err, "get_method_class_name: error in JVMTI GetClassSignature");

  cname[strlen(cname) - 1] = '\0'; // get rid of trailing ';'
  return cname + 1;                // get rid of leading 'L'
}

static void
print_method(jvmtiEnv *jvmti, JNIEnv* jni, jmethodID method, jint depth) {
  char*  cname = NULL;
  char*  mname = NULL;
  char*  msign = NULL;
  jvmtiError err;

  cname = get_method_class_name(jvmti, jni, method);

  err = jvmti->GetMethodName(method, &mname, &msign, NULL);
  check_jvmti_status(jni, err, "print_method: error in JVMTI GetMethodName");

  printf("%2d: %s: %s%s\n", depth, cname, mname, msign);
  fflush(0);
}

static void
print_stack_trace(jvmtiEnv *jvmti, JNIEnv* jni) {
  jvmtiFrameInfo frames[MAX_FRAME_COUNT];
  jint count = 0;
  jvmtiError err;

  err = jvmti->GetStackTrace(NULL, 0, MAX_FRAME_COUNT, frames, &count);
  check_jvmti_status(jni, err, "print_stack_trace: error in JVMTI GetStackTrace");

  printf("JVMTI Stack Trace: frame count: %d\n", count);
  for (int depth = 0; depth < count; depth++) {
    print_method(jvmti, jni, frames[depth].method, depth);
  }
  printf("\n");
}

static void
print_frame_event_info(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method,
                       const char* event_name, int event_count) {
  char* cname = NULL;
  char* mname = NULL;
  char* msign = NULL;
  jvmtiThreadInfo thr_info;
  jvmtiError err;

  memset(&thr_info, 0, sizeof(thr_info));
  err = jvmti->GetThreadInfo(thread, &thr_info);
  check_jvmti_status(jni, err, "event handler: error in JVMTI GetThreadInfo call");
  const char* thr_name = (thr_info.name == NULL) ? "<Unnamed thread>" : thr_info.name;

  cname = get_method_class_name(jvmti, jni, method);

  err = jvmti->GetMethodName(method, &mname, &msign, NULL);
  check_jvmti_status(jni, err, "event handler: error in JVMTI GetMethodName call");

  printf("%s event #%d: thread: %s, method: %s: %s%s\n",
         event_name, event_count, thr_name, cname, mname, msign);

  if (strcmp(event_name, "SingleStep") != 0) {
    print_stack_trace(jvmti, jni);
  }
  fflush(0);
}

static void
print_cont_event_info(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jint frames_cnt, const char* event_name) {
  jvmtiThreadInfo thr_info;
  jvmtiError err;

  memset(&thr_info, 0, sizeof(thr_info));
  err = jvmti->GetThreadInfo(thread, &thr_info);
  check_jvmti_status(jni, err, "event handler failed during JVMTI GetThreadInfo call");

  const char* thr_name = (thr_info.name == NULL) ? "<Unnamed thread>" : thr_info.name;
  printf("\n%s event: thread: %s, frames: %d\n\n", event_name, thr_name, frames_cnt);

  print_stack_trace(jvmti, jni);
  fflush(0);
}

static void
setOrClearBreakpoint(JNIEnv *jni, const char *methodName, jboolean set)
{
  jlocation location = (jlocation)0L;
  jmethodID method = NULL;
  jvmtiError err;
  jint method_count = java_lang_Continuation_method_count;

  // Find the jmethodID of the specified method
  while (--method_count >= 0) {
    jmethodID meth = java_lang_Continuation_methods[method_count];
    char* mname = NULL;

    err = jvmti->GetMethodName(meth, &mname, NULL, NULL);
    check_jvmti_status(jni, err, "setupBreakpoint: error in JVMTI GetMethodName call");

    if (strcmp(mname, methodName) == 0) {
      printf("setupBreakpoint: found method %s() to set a breakpoint\n", mname);
      fflush(0);
      method = meth;
    }
  }
  if (method == NULL) {
      printf("setupBreakpoint: not found method %s() to set a breakpoint\n", methodName);
      jni->FatalError("Error in setupBreakpoint: not found method");
  }

  if (set) {
      err = jvmti->SetBreakpoint(method, location);
  } else {
      err = jvmti->ClearBreakpoint(method, location);
  }
  check_jvmti_status(jni, err, "breakP: error in JVMTI SetBreakpoint");
}

static void
setBreakpoint(JNIEnv *jni, const char *methodName)
{
    setOrClearBreakpoint(jni, methodName, JNI_TRUE);
}

static void
clearBreakpoint(JNIEnv *jni, const char *methodName)
{
    setOrClearBreakpoint(jni, methodName, JNI_FALSE);
}

static jboolean runBreakpointHit = JNI_FALSE;
static jboolean isStartedBreakpointHit = JNI_FALSE;

static void JNICALL
Breakpoint(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread,
           jmethodID method, jlocation location) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "Breakpoint: error in JVMTI GetMethodName call");

  if (strcmp(mname, "run") == 0) {
      // We hit our Continuation.run() breakpoint. Now setup the Continuation.isStarted() breakpoint.
      if (runBreakpointHit) {
          unlock_events();
          return; // ignore if we've already seen one
      }
      print_frame_event_info(jvmti, jni, thread, method,
                             "Breakpoint", ++breakpoint_count);
      runBreakpointHit = JNI_TRUE;
      clearBreakpoint(jni, "run");
      setBreakpoint(jni, "isStarted");
      err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_METHOD_EXIT, thread);
      check_jvmti_status(jni, err, "Breakpoint: error in JVMTI SetEventNotificationMode: enable METHOD_EXIT");
  } else if (strcmp(mname, "isStarted") == 0) {
      // We hit our Continuation.isStarted() breakpoint. Now setup single stepping so we can
      // step into Continuation.doContinue().
      if (isStartedBreakpointHit) {
          unlock_events();
          return; // ignore if we've already seen one
      }
      print_frame_event_info(jvmti, jni, thread, method,
                             "Breakpoint", ++breakpoint_count);
      isStartedBreakpointHit = JNI_TRUE;
      clearBreakpoint(jni, "isStarted");
      err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_SINGLE_STEP, thread);
      check_jvmti_status(jni, err, "Breakpoint: error in JVMTI SetEventNotificationMode: enable SINGLE_STEP");
      err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_BREAKPOINT, NULL);
      check_jvmti_status(jni, err, "Breakpoint: error in JVMTI SetEventNotificationMode: enable BREAKPOINT");
      err = jvmti->NotifyFramePop(thread, 0);
      check_jvmti_status(jni, err, "Breakpoint: error in JVMTI NotifyFramePop0");
  } else {
      printf(" Breakpoint: unexpected breakpoint in method %s()\n", mname);
  }

  unlock_events();
}

static void JNICALL
SingleStep(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread,
           jmethodID method, jlocation location) {
  char* mname = NULL;
  jvmtiError err;

  lock_events();

  err = jvmti->GetMethodName(method, &mname, NULL, NULL);
  check_jvmti_status(jni, err, "SingleStep: error in JVMTI GetMethodName call");

  print_frame_event_info(jvmti, jni, thread, method,
                         "SingleStep", ++single_step_count);
  if (strcmp(mname, "yield0") == 0) {
      // We single stepped into yield0 within 50 steps. Turn off single stepping and let the test complete.
      printf("SingleStep: entered yield0()\n");
      print_frame_event_info(jvmti, jni, thread, method,
                             "SingleStep Passed", single_step_count);
      err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_SINGLE_STEP, thread);
      check_jvmti_status(jni, err, "SingleStep: error in JVMTI SetEventNotificationMode: enable SINGLE_STEP");
      passed = JNI_TRUE;
  } else if (single_step_count >= 50) {
      // We didn't enter Continuation.yield0() within 50 single steps. The test has failed.
      printf("FAILED: SingleStep: never entered method yield0()\n");
      print_frame_event_info(jvmti, jni, thread, method,
                             "SingleStep 50", single_step_count);
      err = jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_SINGLE_STEP, thread);
      check_jvmti_status(jni, err, "SingleStep: error in JVMTI SetEventNotificationMode: enable SINGLE_STEP");
  }
  unlock_events();
}

static void JNICALL
MethodEntry(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method) {
  lock_events();
  method_entry_count++;
  //print_frame_event_info(jvmti, jni, thread, method, "MethodEntry", method_entry_count);
  unlock_events();
}

static void JNICALL
MethodExit(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method,
           jboolean was_popped_by_exception, jvalue return_value) {
  lock_events();
  method_exit_count++;
  //print_frame_event_info(jvmti, jni, thread, method, "MethodExit", method_entry_count);
  unlock_events();
}

static void JNICALL
FramePop(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jmethodID method,
         jboolean was_popped_by_exception) {
  lock_events();
  frame_pop_count++;
  print_frame_event_info(jvmti, jni, thread, method, "FramePop", frame_pop_count);
  unlock_events();
}

static void JNICALL
VirtualThreadScheduled(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jobject fiber) {
  lock_events();
  //processFiberEvent(jvmti, jni, thread, fiber, "VirtualThreadScheduled");
  unlock_events();
}

static void JNICALL
VirtualThreadTerminated(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jobject fiber) {
  lock_events();
  //processFiberEvent(jvmti, jni, thread, fiber, "VirtualThreadTerminated");
  unlock_events();
}

static void JNICALL
VirtualThreadMounted(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jobject fiber) {
  lock_events();
  //processFiberEvent(jvmti, jni, thread, fiber, "VirtualThreadMounted");
  unlock_events();
}

static void JNICALL
VirtualThreadUnmounted(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jobject fiber) {
  lock_events();
  //processFiberEvent(jvmti, jni, thread, fiber, "VirtualThreadUnmounted");
  unlock_events();
}

static void JNICALL
ContinuationRun(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jint frames_count) {
  lock_events();
  //print_cont_event_info(jvmti, jni, thread, frames_count, "ContinuationRun");
  unlock_events();
}

static void JNICALL
ContinuationYield(jvmtiEnv *jvmti, JNIEnv* jni, jthread thread, jint frames_count) {
  lock_events();
  //print_cont_event_info(jvmti, jni, thread, frames_count, "ContinuationYield");
  unlock_events();
}

JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
  jvmtiEventCallbacks callbacks;
  jvmtiCapabilities caps;
  jvmtiError err;

  printf("Agent_OnLoad started\n");
  if (jvm->GetEnv((void **) (&jvmti), JVMTI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.Breakpoint  = &Breakpoint;
  callbacks.SingleStep  = &SingleStep;
  callbacks.FramePop    = &FramePop;
  callbacks.MethodEntry = &MethodEntry;
  callbacks.MethodExit = &MethodExit;
  callbacks.VirtualThreadScheduled  = &VirtualThreadScheduled;
  callbacks.VirtualThreadTerminated = &VirtualThreadTerminated;
  callbacks.VirtualThreadMounted   = &VirtualThreadMounted;
  callbacks.VirtualThreadUnmounted = &VirtualThreadUnmounted;
  callbacks.ContinuationRun   = &ContinuationRun;
  callbacks.ContinuationYield = &ContinuationYield;

  memset(&caps, 0, sizeof(caps));
  caps.can_generate_breakpoint_events = 1;
  caps.can_generate_single_step_events = 1;
  caps.can_generate_frame_pop_events = 1;
  caps.can_generate_method_entry_events = 1;
  caps.can_generate_method_exit_events = 1;
  caps.can_support_virtual_threads = 1;
  caps.can_support_continuations = 1;

  err = jvmti->AddCapabilities(&caps);
  if (err != JVMTI_ERROR_NONE) {
    printf("Agent_OnLoad: Error in JVMTI AddCapabilities: %d\n", err);
  }

  err = jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks));
  if (err != JVMTI_ERROR_NONE) {
    printf("Agent_OnLoad: Error in JVMTI SetEventCallbacks: %d\n", err);
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VIRTUAL_THREAD_SCHEDULED, NULL);
  if (err != JVMTI_ERROR_NONE) {
    printf("error in JVMTI SetEventNotificationMode: %d\n", err);
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VIRTUAL_THREAD_TERMINATED, NULL);
  if (err != JVMTI_ERROR_NONE) {
    printf("error in JVMTI SetEventNotificationMode: %d\n", err);
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VIRTUAL_THREAD_MOUNTED, NULL);
  if (err != JVMTI_ERROR_NONE) {
    printf("error in JVMTI SetEventNotificationMode: %d\n", err);
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VIRTUAL_THREAD_UNMOUNTED, NULL);
  if (err != JVMTI_ERROR_NONE) {
    printf("error in JVMTI SetEventNotificationMode: %d\n", err);
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CONTINUATION_RUN, NULL);
  if (err != JVMTI_ERROR_NONE) {
      printf("error in JVMTI SetEventNotificationMode: %d\n", err);
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CONTINUATION_YIELD, NULL);
  if (err != JVMTI_ERROR_NONE) {
      printf("error in JVMTI SetEventNotificationMode: %d\n", err);
  }

  err = jvmti->CreateRawMonitor("Events Monitor", &event_mon);
  if (err != JVMTI_ERROR_NONE) {
    printf("Agent_OnLoad: Error in JVMTI CreateRawMonitor: %d\n", err);
  }

  printf("Agent_OnLoad finished\n");
  fflush(0);

  return JNI_OK;
}

JNIEXPORT void JNICALL
Java_DoContinueSingleStepTest_enableEvents(JNIEnv *jni, jclass klass, jthread thread, jclass contKlass) {
  jvmtiError err;

  printf("enableEvents: started\n");

  java_lang_Continuation_class = (jclass)jni->NewGlobalRef(contKlass);
  err = jvmti->GetClassMethods(contKlass, &java_lang_Continuation_method_count, &java_lang_Continuation_methods);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI GetClassMethods");

  setBreakpoint(jni, "run");

  // Enable Breakpoint events globally
  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_BREAKPOINT, NULL);
  check_jvmti_status(jni, err, "enableEvents: error in JVMTI SetEventNotificationMode: enable BREAKPOINT");

  printf("enableEvents: finished\n");
  fflush(0);
}

JNIEXPORT jboolean JNICALL
Java_DoContinueSingleStepTest_check(JNIEnv *jni, jclass cls) {
  printf("\n");
  printf("check: started\n");

  printf("check: breakpoint_count:   %d\n", breakpoint_count);
  printf("check: single_step_count:  %d\n", single_step_count);

  printf("check: finished\n");
  printf("\n");
  fflush(0);

  return passed;
}
} // extern "C"
