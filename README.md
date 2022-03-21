# ntpc

Very simple NTP client with CAP_SYS_TIME support; it's like helloworld.c that
demonstrates usage of NTP and CAP_SYS_TIME.

If ntpc binary is granted CAP_SYS_TIME (by `setcap cap_sys_time+p /path/to/ntpc`),
it'd be able to run under regular user accounts and perform its duties just fine,
without relying on root or suid-root bit.

## ntpc and grsec

When using ntpc together with grsec RBAC, minimal permissions for ntpc subject
in /etc/grsec/policy are as follows (assuming that local DNS server is
listening on 127.0.0.1:53):
```
subject /path/to/ntpc {
	/			h
	/etc/gai.conf		r
	/etc/services		r
	$libattr_
	$libc_
	$libcap_
	$libm_
	$libnss_db_
	$libnss_dns_
	$libresolv_
	-CAP_ALL
	+CAP_SYS_TIME
	bind	disabled
	connect	127.0.0.1/32:53	dgram	udp
	connect	0.0.0.0/0:123	dgram	udp
	connect	0.0.0.0/0:123	stream	tcp
}
```
