# Accepted-connection fds must be close-on-exec: an inherited fd keeps the
# client blocked in read() until the child exits (see server_conn_io.c).
$(TESTPREFIX)/unit-test-server-conn-accept: $(OBJDIR)/tests/test_server_conn_accept.o \
                                           $(OBJDIR)/server/server_conn_io.o $(PLATFORM_BASIC_OBJS)
	$(TESTLINK) -o $@ $^ $(L_CORE)
