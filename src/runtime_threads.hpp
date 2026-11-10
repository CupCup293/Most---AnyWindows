#pragma once
inline constexpr auto runtimeThreads = R"MOST(
struct GuestThread {
    bool exited = false;
};
static std::map<uint32_t, std::shared_ptr<GuestThread>> guest_thread_handles;
static uint32_t next_guest_thread = 0x60000000u;
static unsigned active_guest_threads;

static bool close_thread_handle(uint32_t handle)
{
    return guest_thread_handles.erase(handle) != 0;
}

static void guest_checkpoint()
{
#ifdef MOST_GUEST_THREADS
    static thread_local unsigned steps;
    if (active_guest_threads && ++steps == 4096) {
        steps = 0;
        const GuestBlocking blocking;
        std::this_thread::yield();
    }
#endif
}

static uint32_t win_CreateThread(uint32_t* sp)
{
    const auto attributes = pop32(sp), stack_size = pop32(sp), start = pop32(sp);
    const auto parameter = pop32(sp), flags = pop32(sp), id = pop32(sp);
#ifndef MOST_GUEST_THREADS
    return process_error(50);
#else
    if (attributes || flags || stack_size > 8 * 1024 * 1024 || !is_guest_callback(start))
        return process_error(50);
    if (active_guest_threads >= 32 || guest_thread_handles.size() >= 4096 || next_guest_thread == 0x70000000u)
        return process_error(8);
    if (id)
        memory(id, 4, 1);
    const auto handle = next_guest_thread++;
    const auto stack_capacity = std::max(1024 * 1024u, stack_size);
    uint32_t base = 0xd0000000u;
    while (guest_thread_stacks.contains(base))
        base += 8 * 1024 * 1024;
    auto state = std::make_shared<GuestThread>();
    try {
        guest_thread_stacks.emplace(base, std::vector<uint8_t>(stack_capacity));
        guest_thread_handles.emplace(handle, state);
        ++active_guest_threads;
        try {
            std::thread([state, start, parameter, base, stack_capacity] {
                guest_main_thread = false;
                std::lock_guard execution(guest_execution_mutex);
                uint32_t frame = base + stack_capacity;
                push32(&frame, parameter);
                push32(&frame, 0);
                try {
                    run_guest_code(start, frame);
                } catch (const GuestExit& exit) {
                    fflush(nullptr);
                    _Exit(exit.status);
                }
                guest_thread_stacks.erase(base);
                state->exited = true;
                --active_guest_threads;
            }).detach();
        } catch (...) {
            --active_guest_threads;
            throw;
        }
    } catch (const std::exception&) {
        guest_thread_handles.erase(handle);
        guest_thread_stacks.erase(base);
        return process_error(8);
    }
    if (id)
        store32(id, handle);
    return handle;
#endif
}

static uint32_t wait_guest_thread(uint32_t handle, uint32_t timeout)
{
    const auto entry = guest_thread_handles.find(handle);
    if (entry == guest_thread_handles.end()) {
        last_error = 6;
        return ~0u;
    }
    const auto thread = entry->second;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (!thread->exited) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (timeout != ~0u && remaining <= std::chrono::steady_clock::duration::zero())
            return 258;
        const GuestBlocking blocking;
        if (timeout == ~0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            std::this_thread::sleep_for(std::min(remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(2))));
        }
    }
    return 0;
}
)MOST";
