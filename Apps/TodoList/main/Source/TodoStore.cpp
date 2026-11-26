#include "TodoStore.h"

#include <app/paths.h>
#include <tactility/log.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static constexpr auto* TAG = "TodoList";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "tactility.todolist";

static constexpr size_t MAX_FILE_SIZE = 16 * 1024;

static std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) return names;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] != '.') {
            names.emplace_back(entry->d_name);
        }
    }
    closedir(dir);
    return names;
}

static bool isDirectory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

static bool fileExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

static std::string lowerCase(const std::string& text) {
    std::string result = text;
    for (char& c : result) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return result;
}

/** @return the id of "<id>.task", or 0 for other files */
static uint32_t idOfFileName(const std::string& name) {
    const size_t dot = name.find('.');
    if (dot == std::string::npos || name.compare(dot, std::string::npos, ".task") != 0) return 0;
    return static_cast<uint32_t>(strtoul(name.c_str(), nullptr, 10));
}

static bool readFile(const std::string& path, std::string& content) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    content.clear();
    char buffer[256];
    size_t length;
    while (content.size() < MAX_FILE_SIZE && (length = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, length);
    }
    fclose(file);
    return true;
}

/** Writes a temporary file and then renames it, so a task isn't lost when writing fails */
static bool writeFile(const std::string& path, const std::string& content) {
    const std::string temporary = path + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (file == nullptr) return false;
    bool success = fwrite(content.data(), 1, content.size(), file) == content.size();
    success = (fclose(file) == 0) && success;
    if (!success) {
        unlink(temporary.c_str());
        return false;
    }
    // FAT can't rename onto an existing file
    unlink(path.c_str());
    return rename(temporary.c_str(), path.c_str()) == 0;
}

static Task parseTask(const std::string& content) {
    Task task;
    size_t position = 0;
    while (position < content.size()) {
        size_t end = content.find('\n', position);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(position, end - position);
        position = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break; // the details follow

        const size_t colon = line.find(": ");
        const std::string key = colon == std::string::npos ? line : line.substr(0, colon);
        const std::string value = colon == std::string::npos ? "" : line.substr(colon + 2);
        if (key == "title") task.title = value;
        else if (key == "done") task.done = value == "1";
        else if (key == "created") task.created = static_cast<long long>(strtoul(value.c_str(), nullptr, 10));
        else if (key == "completed") task.completed = static_cast<long long>(strtoul(value.c_str(), nullptr, 10));
        else if (key != "version") task.unknownLines.push_back(line);
    }
    if (position < content.size()) {
        task.details = content.substr(position);
    }
    return task;
}

static std::string formatTask(const Task& task) {
    std::string title = task.title;
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');

    char numbers[96];
    snprintf(numbers, sizeof(numbers), "done: %d\ncreated: %lld\ncompleted: %lld\n", task.done ? 1 : 0, task.created, task.completed);

    std::string content = "version: 1\ntitle: " + title + "\n" + numbers;
    for (const auto& line : task.unknownLines) {
        content += line + "\n";
    }
    content += "\n" + task.details;
    return content;
}

std::string TodoStore::taskPath(const std::string& group, uint32_t id) const {
    return groupPath(group) + "/" + std::to_string(id) + ".task";
}

bool TodoStore::init() {
    char directory[256];
    if (app_paths_get_user_data_directory(APP_ID, directory, sizeof(directory)) != ERROR_NONE) return false;
    for (char* p = directory + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(directory, 0755); *p = '/'; }
    }
    mkdir(directory, 0755);
    root = directory;
    while (!root.empty() && root.back() == '/') root.pop_back();
    mkdir((root + "/tasks").c_str(), 0755);
    if (!isDirectory(root + "/tasks")) {
        LOG_E(TAG, "Can't create the task directory");
        return false;
    }

    for (const auto& group : listDirectory(root + "/tasks")) {
        for (const auto& name : listDirectory(groupPath(group))) {
            nextId = std::max(nextId, idOfFileName(name) + 1);
        }
    }
    migrate();
    return true;
}

void TodoStore::migrate() {
    const std::string old = root + "/todos.txt";
    std::string content;
    if (!readFile(old, content)) return;

    createGroup(DEFAULT_GROUP);
    // Lines of "- title" for a pending task and "+ title" for a completed one
    size_t position = 0;
    while (position < content.size()) {
        size_t end = content.find('\n', position);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(position, end - position);
        position = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != ' ') continue;

        Task task;
        task.done = line[0] == '+';
        task.title = line.substr(2);
        if (!saveTask(DEFAULT_GROUP, task)) {
            LOG_E(TAG, "Migration failed, keeping todos.txt");
            return;
        }
    }

    const std::string backup = old + ".bak";
    unlink(backup.c_str());
    rename(old.c_str(), backup.c_str());
}

std::vector<std::string> TodoStore::loadGroups() {
    std::vector<std::string> groups;
    for (const auto& name : listDirectory(root + "/tasks")) {
        if (isDirectory(groupPath(name))) groups.push_back(name);
    }
    if (groups.empty() && createGroup(DEFAULT_GROUP)) {
        groups.emplace_back(DEFAULT_GROUP);
    }
    std::sort(groups.begin(), groups.end(), [](const std::string& a, const std::string& b) {
        const bool aDefault = a == DEFAULT_GROUP;
        const bool bDefault = b == DEFAULT_GROUP;
        if (aDefault != bDefault) return aDefault;
        return lowerCase(a) < lowerCase(b);
    });
    return groups;
}

std::string TodoStore::cleanGroupName(const std::string& name) {
    std::string result;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) >= ' ' && strchr("/\\:*?\"<>|", c) == nullptr) {
            result += c;
        }
    }
    while (!result.empty() && (result.front() == ' ' || result.front() == '.')) result.erase(result.begin());
    if (result.size() > MAX_GROUP_NAME) result.resize(MAX_GROUP_NAME);
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    return result;
}

bool TodoStore::createGroup(const std::string& name) {
    return !name.empty() && mkdir(groupPath(name).c_str(), 0755) == 0;
}

bool TodoStore::renameGroup(const std::string& from, const std::string& to) {
    if (to.empty()) return false;
    // A change of only the case is the same directory on FAT
    if (lowerCase(from) != lowerCase(to) && fileExists(groupPath(to))) return false;
    return rename(groupPath(from).c_str(), groupPath(to).c_str()) == 0;
}

bool TodoStore::deleteGroup(const std::string& name) {
    for (const auto& file : listDirectory(groupPath(name))) {
        unlink((groupPath(name) + "/" + file).c_str());
    }
    return rmdir(groupPath(name).c_str()) == 0;
}

std::vector<Task> TodoStore::loadTasks(const std::string& group) {
    // Finish saves that were interrupted between removing the old file and renaming the new one
    for (const auto& name : listDirectory(groupPath(group))) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            const std::string temporary = groupPath(group) + "/" + name;
            const std::string target = temporary.substr(0, temporary.size() - 4);
            if (fileExists(target)) unlink(temporary.c_str());
            else rename(temporary.c_str(), target.c_str());
        }
    }

    std::vector<Task> tasks;
    for (const auto& name : listDirectory(groupPath(group))) {
        const uint32_t id = idOfFileName(name);
        std::string content;
        if (id == 0 || !readFile(taskPath(group, id), content)) continue;
        Task task = parseTask(content);
        task.id = id;
        tasks.push_back(std::move(task));
    }
    std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) { return a.id < b.id; });
    return tasks;
}

bool TodoStore::saveTask(const std::string& group, Task& task) {
    if (task.id == 0) {
        task.id = nextId++;
        const time_t now = time(nullptr);
        task.created = static_cast<long long>(now);
    }
    return writeFile(taskPath(group, task.id), formatTask(task));
}

bool TodoStore::deleteTask(const std::string& group, uint32_t id) {
    return unlink(taskPath(group, id).c_str()) == 0;
}

bool TodoStore::moveTask(const std::string& from, const std::string& to, uint32_t id) {
    return rename(taskPath(from, id).c_str(), taskPath(to, id).c_str()) == 0;
}
