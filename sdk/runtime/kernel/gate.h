// [local change, The Suffering: Ties That Bind] The critical-section gate.
//
// The console has one processor: while a thread is between an EnterCriticalSection and its LeaveCriticalSection no
// other thread runs, unless the first one waits for something, or its time slice ends, or an interrupt wakes a
// thread of higher priority. Titles lean on that. This game's main thread takes the resource table lock (0x3986AC,
// in 0x185E50) and then the pending-load lock (0x398A44, in 0x18A9A0), while its loader takes them the other way
// round (0x1904C0 holds 0x398A44 and calls 0x192E10, which takes 0x3986AC); tools\lockorder.py finds eight such
// pairs in the image. On a PC's many processors the two threads really run at once, each takes its first lock,
// and each then waits for the other for good: about one level load in six to ten froze.
//
// The gate brings the console's rule back for exactly that case: of the threads that are inside a critical section
// of the title, one runs at a time. A thread takes the gate when it enters its first critical section and gives it
// back when it has left its last one; it also gives it up while it waits, sleeps or reads a file inside a critical
// section, as the console would switch to another thread there. It goes to the waiting threads in the order they
// asked, so a thread that enters and leaves sections in a tight loop cannot keep the others out. Code outside
// critical sections is not held back: the title's threads keep their own processors.
//
// The wait for the gate is bounded (3 ms). The title's critical sections last microseconds, but its main thread
// also sits inside one in a loop that only reads memory (0x15E7C2: while the resource's state is "loading",
// look again) until the file thread, which needs a critical section of its own, has finished the load. On the
// console the file thread has the higher priority (SetThreadPriority +1 at 0x1E71D1) and cuts in. Here a thread
// that has waited 3 ms goes ahead without the gate, and nobody waits again for that same stay of the holder. So
// the gate can delay a thread by 3 ms at most and can never stop the game by itself; in the worst case the
// threads run as they would without it. Each such case is counted in the log ("gate: thread ... goes ahead").
//
// Why not one processor for all title threads (tried first): Windows lends a real-time thread's priority to the
// owner of a lock that thread waits for (heap, loader and runtime locks; the sound threads are real-time). The
// main thread got 24 that way while it sat in a loop that never pauses (0x15E890, waiting for a load), and with
// every title thread on its processor nothing else of the title ever ran again: the start-up hang.
#pragma once

namespace kernel {

void GateEnterSection();  // before a critical section is entered
void GateLeaveSection();  // after one is left
bool GateTryEnterSection();           // for RtlTryEnterCriticalSection: call before the try...
void GateTryFailed(bool took_gate);   // ...and this if the try failed
// The thread is about to block inside a critical section: another thread may use the gate meanwhile. Returns
// whether it gave the gate up; GateAfterBlock takes it back (never call that while holding a runtime lock that a
// thread inside a critical section may ask for).
bool GateBeforeBlock();
void GateAfterBlock();
void GateThreadExit();
int GateDepth();  // critical sections the calling thread is inside

struct GateBlocking {
    bool out = GateBeforeBlock();
    ~GateBlocking() {
        if (out) GateAfterBlock();
    }
};

}  // namespace kernel
