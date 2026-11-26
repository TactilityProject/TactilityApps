# Todo List

Tasks in groups, each with a title and free-form details.

## Controls

| Action | Result |
|--------|--------|
| Type in the field at the bottom, press Enter or + | Add a task to the selected group |
| Tap the box, or Space | Finish or reopen the task |
| Tap the task, or Enter | Open its details; changes are saved when you go back |
| Long press the task | Move to another group, or delete |
| Delete or Backspace key | Delete the focused task, after a confirmation |
| The **+** tab | New group |
| Long press a group tab | Rename or delete the group |
| Menu button | Show or hide finished tasks, delete them all, or delete the group (not the only one) |

## Storage

One file per task, in a directory per group inside the app's user data directory:
`tasks/<group>/<id>.task`

```
version: 1
title: Order solder wick
done: 0
created: 1795600000
completed: 0

The details, as plain text.
```

Header lines that the app doesn't know are kept when it rewrites a file.

The `todos.txt` of earlier versions is moved into the **General** group, and kept as `todos.txt.bak`.
