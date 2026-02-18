# Todo List

A simple task list manager for Tactility.

## Overview

Keep track of tasks directly on your device. Add items, mark them complete, and delete them when done. Tasks are automatically saved to the SD card and persist across sessions.

## Features

- Add, complete, and delete tasks
- Visual completion indicators (checkmarks vs pending dots)
- Strike-through styling for completed items
- Pending task counter in header
- Clear all completed tasks at once
- Persistent storage on SD card (survives reboots)

## Controls

- **Touch**:
  - Tap a task: Toggle between done and pending
  - Tap X button: Delete a task
- **Text Input**: Type a task name, then press Enter or the + button to add it
- **Toolbar Button**: Trash icon to clear all completed tasks

## Storage

Tasks are saved to `/sdcard/tactility/todolist/todos.txt` in a simple text format:
- `-` prefix = pending task
- `+` prefix = completed task

Up to 50 tasks, 128 characters each.

## Requirements

- Tactility
- SD card (for persistent storage)
- Touchscreen or keyboard
