#pragma once

// App icon resource id.
// Keep this file ASCII-only: rc.exe parses .h files with the ANSI code page, and
// UTF-8 Chinese comments without a BOM can swallow the following #define line,
// which silently turns the resource into a string-named one ("IDI_APP") and
// breaks MAKEINTRESOURCEW(IDI_APP) lookups at runtime.
#define IDI_APP 101
