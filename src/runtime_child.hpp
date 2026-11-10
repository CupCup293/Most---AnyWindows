#pragma once
inline constexpr auto runtimeChild = R"MOST(
struct GuestChild {
    pid_t pid = 0;
    uint32_t exit_code = 259;
    bool exited = false;
};
struct GuestChildHandle {
    std::shared_ptr<GuestChild> child;
    bool process;
};
static std::map<uint32_t, GuestChildHandle> child_handles;
static std::vector<std::shared_ptr<GuestChild>> children;
static uint32_t next_child_handle = 0x50000000u;

static bool refresh_child(GuestChild& child, bool block)
{
    if (child.exited)
        return true;
    int status;
    pid_t result;
    do {
        result = waitpid(child.pid, &status, block ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        last_error = 6;
        return false;
    }
    if (result) {
        child.exited = true;
        child.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    return true;
}

static bool close_process_handle(uint32_t handle)
{
    const auto entry = child_handles.find(handle);
    if (entry == child_handles.end())
        return false;
    auto child = entry->second.child;
    child_handles.erase(entry);
    refresh_child(*child, false);
    return true;
}

static std::vector<std::u16string> parse_child_arguments(std::u16string_view line)
{
    std::vector<std::u16string> arguments;
    while (!line.empty() && arguments.size() < 4096) {
        const auto first = line.find_first_not_of(u" \t");
        if (first == line.npos)
            break;
        line.remove_prefix(first);
        auto& argument = arguments.emplace_back();
        bool quoted = false;
        while (!line.empty()) {
            if (!quoted && (line.front() == u' ' || line.front() == u'\t'))
                break;
            size_t slashes = 0;
            while (!line.empty() && line.front() == u'\\') {
                ++slashes;
                line.remove_prefix(1);
            }
            if (!line.empty() && line.front() == u'"') {
                argument.append(slashes / 2, u'\\');
                line.remove_prefix(1);
                if (slashes % 2) {
                    argument += u'"';
                } else if (quoted && !line.empty() && line.front() == u'"') {
                    argument += u'"';
                    line.remove_prefix(1);
                } else {
                    quoted = !quoted;
                }
            } else {
                argument.append(slashes, u'\\');
                if (line.empty() || (!quoted && (line.front() == u' ' || line.front() == u'\t')))
                    break;
                argument += line.front();
                line.remove_prefix(1);
            }
        }
    }
    return arguments;
}

static uint32_t process_error(uint32_t error)
{
    last_error = error;
    return 0;
}

static int spawn_guest_elf(const std::filesystem::path& path, std::vector<std::string>& args,
                           const std::filesystem::path& directory, pid_t& pid)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno;
    const auto close_fd = [](const int* value) { close(*value); };
    const std::unique_ptr<const int, decltype(close_fd)> guard(&fd, close_fd);
    struct stat info{};
    uint8_t magic[4];
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
        || read(fd, magic, sizeof magic) != sizeof magic || memcmp(magic, "\x7f" "ELF", 4))
        return ENOEXEC;
    const auto caps = fgetxattr(fd, "security.capability", nullptr, 0);
    if ((info.st_mode & (S_ISUID | S_ISGID)) || caps > 0 || (caps < 0 && errno != ENODATA && errno != ENOTSUP))
        return EACCES;
    if (fd < 3 || fd >= 4096)
        return EMFILE;
    const auto executable = "/proc/self/fd/" + std::to_string(fd);
    std::vector<char*> argv;
    for (auto& value : args)
        argv.push_back(value.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result)
        return result;
    for (int other = 3; !result && other < fd; ++other)
        result = posix_spawn_file_actions_addclose(&actions, other);
    if (!result)
        result = posix_spawn_file_actions_addclosefrom_np(&actions, fd + 1);
    if (!result && !directory.empty())
        result = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    if (!result)
        result = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return result;
}

static uint32_t launch_error(int error)
{
    if (error == ENOEXEC)
        return 193;
    if (error == ENOMEM || error == EAGAIN)
        return 8;
    return file_error(error);
}

static uint32_t win_CreateProcessW(uint32_t* sp)
{
    const auto application = pop32(sp), command = pop32(sp);
    const auto process_attributes = pop32(sp), thread_attributes = pop32(sp);
    const auto inherit = pop32(sp), flags = pop32(sp), env = pop32(sp), cwd = pop32(sp);
    const auto startup = pop32(sp), output = pop32(sp);
    if (!startup || !output || (!application && !command))
        return process_error(87);
    if (process_attributes || thread_attributes || inherit || env || (flags & ~0x08000000u))
        return process_error(50);
    if (load32(startup) < 68 || (load32(startup + 44) & ~1u))
        return process_error(50);
    auto* info = memory(output, 16, 1);
    std::u16string line;
    if (command && !read_guest_wide(command, line))
        return 0;
    const auto words = parse_child_arguments(line);
    if (words.size() >= 4096 || (!application && words.empty()))
        return process_error(87);
    auto path = application ? guest_path(application) : resolve_guest_path(words.front());
    auto directory = cwd ? guest_path(cwd) : std::filesystem::path();
    if (path.empty() || (cwd && directory.empty()))
        return 0;
    std::vector<std::string> args;
    try {
        for (const auto& word : words)
            args.push_back(std::filesystem::path(word).string());
    } catch (const std::filesystem::filesystem_error&) {
        return process_error(87);
    }
    if (args.empty())
        args.push_back(path.string());
    std::erase_if(children, [](const auto& child) {
        return refresh_child(*child, false) && child->exited;
    });
    if (children.size() >= 1024 || child_handles.size() >= 4094 || next_child_handle >= 0x5fffffffu)
        return process_error(8);

    auto child = std::make_shared<GuestChild>();
    const auto process = next_child_handle++, thread = next_child_handle++;
    children.push_back(child);
    child_handles.emplace(process, GuestChildHandle{child, true});
    child_handles.emplace(thread, GuestChildHandle{child, false});
    const int result = spawn_guest_elf(path, args, directory, child->pid);
    if (result) {
        child_handles.erase(process);
        child_handles.erase(thread);
        children.pop_back();
        return process_error(launch_error(result));
    }
    const uint32_t values[] = {process, thread, uint32_t(child->pid), uint32_t(child->pid)};
    memcpy(info, values, sizeof values);
    return 1;
}

static uint32_t win_GetExitCodeProcess(uint32_t* sp)
{
    const auto handle = pop32(sp), output = pop32(sp);
    const auto entry = child_handles.find(handle);
    if (entry == child_handles.end() || !entry->second.process)
        return process_error(6);
    if (!output)
        return process_error(87);
    auto& child = *entry->second.child;
    if (!refresh_child(child, false))
        return 0;
    store32(output, child.exit_code);
    return 1;
}

static uint32_t wait_guest_thread(uint32_t handle, uint32_t timeout);

static uint32_t win_WaitForSingleObject(uint32_t* sp)
{
    const auto handle = pop32(sp), timeout = pop32(sp);
    const auto entry = child_handles.find(handle);
    if (entry == child_handles.end())
        return wait_guest_thread(handle, timeout);
    const auto child = entry->second.child;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (refresh_child(*child, false)) {
        if (child->exited)
            return 0;
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            return 258;
        const GuestBlocking blocking;
        std::this_thread::sleep_for(std::min(remaining,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(5))));
    }
    return ~0u;
}
)MOST";
