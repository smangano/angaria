# Angaria C++ conventions

- Use CamelCase for class and struct names, such as `MessageQueue`.
- Prefix member variables with an underscore and use camelCase, such as `_messageCount`.
- Use underscore-separated words for functions and member functions, such as `receive_message`.
- Use Allman formatting, with braces on their own lines aligned with the declaration or control statement.
- Include project headers with `#include "..."`.
- Include standard-library and third-party headers with `#include <...>`.

Boost and Intel TBB are permitted dependencies when warranted by the implementation.
