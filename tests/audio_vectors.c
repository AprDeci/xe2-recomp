/* Run the runtime's synthetic vectors without loading a game or opening waveOut. */
#include "../runtime/ipc_audioout.c"

/* Guest hooks are unavailable in this game-free harness; any accidental call must fail. */
int rt_trace;
void f_71091796f0_orig(Cpu *c) { (void)c; abort(); }
void f_7109179a6c_orig(Cpu *c) { (void)c; abort(); }
void f_71091798fc_orig(Cpu *c) { (void)c; abort(); }

int main(void) {
    InitializeCriticalSection(&rlock);
    int failures = audio_vectors();
    DeleteCriticalSection(&rlock);
    return failures ? 1 : 0;
}
