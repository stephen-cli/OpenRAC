/* pthreads-embedded declares pthread_num_processors_np but its PS3 platform does not define it;
 * libstdc++'s std::thread::hardware_concurrency calls it. The Cell's PPU runs two hardware
 * threads. */
int pthread_num_processors_np(void) {
    return 2;
}
