#pragma once

// Tasks are stored as one file each, in a directory per group:
//   <user data>/tasks/<group>/<id>.task
// A file has "key: value" header lines, an empty line, and then the details as plain text.
// Header lines with unknown keys are kept as they are, so newer fields survive.

#include <stdint.h>
#include <string>
#include <vector>

struct Task {
    uint32_t id = 0; // unique across all groups, and the order of the tasks
    std::string title;
    std::string details;
    bool done = false;
    long long created = 0;   // seconds since 1970
    long long completed = 0; // 0 when not done
    std::vector<std::string> unknownLines;
};

constexpr size_t MAX_GROUP_NAME = 24;
constexpr const char* DEFAULT_GROUP = "General";

class TodoStore {

public:

    /** Creates the directories and migrates the todos.txt of earlier versions */
    bool init();

    /** @return the group names: the default group first, the others alphabetically. At least one. */
    std::vector<std::string> loadGroups();

    /** @return the name without characters that can't be in a directory name, possibly empty */
    static std::string cleanGroupName(const std::string& name);

    bool createGroup(const std::string& name);
    bool renameGroup(const std::string& from, const std::string& to);
    /** Deletes the group and its tasks */
    bool deleteGroup(const std::string& name);

    std::vector<Task> loadTasks(const std::string& group);
    /** Gives a new task (id 0) its id */
    bool saveTask(const std::string& group, Task& task);
    bool deleteTask(const std::string& group, uint32_t id);
    bool moveTask(const std::string& from, const std::string& to, uint32_t id);

    /** Where the app keeps its settings */
    std::string getSettingsPath() const { return root + "/settings.properties"; }

private:

    std::string root;
    uint32_t nextId = 1;

    std::string groupPath(const std::string& group) const { return root + "/tasks/" + group; }
    std::string taskPath(const std::string& group, uint32_t id) const;
    void migrate();
};
