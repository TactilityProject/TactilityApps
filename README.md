# Tactility Apps

This project contains various official Tactility apps. Some are usable day-to-day while others are simply feature showcases.

# Publishing

The apps are built and published by [TactilityAppPublisher](https://github.com/TactilityProject/TactilityAppPublisher), which is a git submodule in `TactilityAppPublisher/`. Clone with `--recurse-submodules`, or run `git submodule update --init`.
`.github/workflows/main.yml` calls its reusable workflow. To run its tools locally, for example to bump versions or to check a branch:

```
python TactilityAppPublisher/apppublisher.py bump patch
python TactilityAppPublisher/apppublisher.py check-versions main
```

# License

Unless otherwise specified, the subprojects in the TactilityApps project are available under the [GNU General Public License v3](Documentation/license-apps.md).
Some applications have a different license, which is specified by a `LICENSE` or `LICENSE.md` file in their folder.

The [Libraries](./Libraries) have their own license. Read more about licensing in [LICENSE.md](LICENSE.md).
