LDLIBS=-lm -lcap

ntpc: ntpc.c

# NOTES:
#
# ntpc binary should be granted "CAP_SYS_TIME,p" capability after
# compiling/building, e.g.:
#     setcap cap_sys_time+p ./ntpc
#
# When using ntpc together with grsec RBAC, minimal permissions for
# ntpc subject in /etc/grsec/policy are as follows:
#     subject /path/to/ntpc {
#	    /                               h
#	    /etc/gai.conf                   r
#	    /etc/services                   r
#	    $libattr_
#	    $libc_
#	    $libcap_
#	    $libm_
#	    $libnss_db_
#	    $libnss_dns_
#	    $libresolv_
#	    -CAP_ALL
#	    +CAP_SYS_TIME
#	    bind    disabled
#	    connect 127.0.0.1/32:53 dgram   udp
#	    connect 0.0.0.0/0:123   dgram   udp
#	    connect 0.0.0.0/0:123   stream  tcp
#     }
