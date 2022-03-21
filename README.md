# ntpc

Very simple NTP client with CAP_SYS_TIME support; it's like helloworld.c that
demonstrates usage of NTP and CAP_SYS_TIME.

If ntpc binary is granted CAP_SYS_TIME (by `setcap cap_sys_time+p /path/to/ntpc`),
it'd be able to run under regular user accounts and perform its duties just fine,
without relying on root or suid-root bit.
