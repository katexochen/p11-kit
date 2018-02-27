/*
 * Copyright (c) 2018 Red Hat Inc
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Author: Daiki Ueno
 */

#include "config.h"
#include "test.h"

#include "dict.h"
#include "library.h"
#include "filter.h"
#include "mock.h"
#include "modules.h"
#include "p11-kit.h"
#include "remote.h"
#include "virtual.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct {
	char *directory;
	char *socket_path;
	pid_t pid;
} test;

static void
setup_server (void *arg)
{
	char *argv[] = {
		"p11-kit-server",
		"-f",
		"--provider",
		BUILDDIR "/.libs/mock-one" SHLEXT,
		"-n",
		NULL,
		NULL,
		NULL
	};
	char *address;
	int fds[2];
	struct pollfd pfd;
	int ret;
	const char *envvar;
	char *path;

	test.directory = p11_test_directory ("p11-test-server");
	if (asprintf (&path, "%s/p11-kit", test.directory) < 0)
		assert_not_reached ();
	if (mkdir (path, 0700) < 0)
		assert_not_reached ();
	if (asprintf (&test.socket_path, "%s/pkcs11", path) < 0)
		assert_not_reached ();
	free (path);
	unlink (test.socket_path);

	ret = socketpair (AF_UNIX, SOCK_STREAM, 0, fds);
	assert_num_cmp (-1, !=, ret);

	setenv ("P11_KIT_PRIVATEDIR", BUILDDIR, 1);

	/* Allow the child process to preload libasan.so */
	envvar = secure_getenv ("P11_KIT_TEST_LD_PRELOAD");
	if (envvar)
		setenv ("LD_PRELOAD", envvar, 1);

	argv[5] = test.socket_path;
	argv[6] = (char *)arg;

	test.pid = fork ();

	/* The child */
	if (test.pid == 0) {
		close (STDOUT_FILENO);
		dup2 (fds[0], STDOUT_FILENO);
		execv (BUILDDIR "/p11-kit-server", argv);
		_exit (0);
	}

	memset (&pfd, 0, sizeof (struct pollfd));
	pfd.fd = fds[1];
	pfd.events = POLLIN | POLLHUP | POLLERR;
	ret = poll (&pfd, 1, 10000);
	assert_num_cmp (-1, !=, ret);

	close (fds[0]);
	close (fds[1]);

	if (asprintf (&address, "unix:path=%s", test.socket_path) < 0)
		assert_not_reached ();
	setenv ("P11_KIT_SERVER_ADDRESS", address, 1);
	free (address);
}

static void
teardown_server (void *unused)
{
	int status;
	kill (test.pid, SIGKILL);
	waitpid (test.pid, &status, 0);

	p11_test_directory_delete (test.directory);
	free (test.directory);
	free (test.socket_path);
}

static void
test_initialize (void *unused)
{
	CK_FUNCTION_LIST_PTR module;
	CK_RV rv;

	module = p11_kit_module_load (BUILDDIR "/.libs/p11-kit-client" SHLEXT, 0);
	assert (module != NULL);

	rv = p11_kit_module_initialize (module);
	assert (rv == CKR_OK);

	rv = p11_kit_module_finalize (module);
	assert (rv == CKR_OK);

	p11_kit_module_release (module);
}

static void
test_initialize_no_address (void *unused)
{
	CK_FUNCTION_LIST_PTR module;
	CK_RV rv;

	unsetenv ("P11_KIT_SERVER_ADDRESS");
	setenv ("XDG_RUNTIME_DIR", test.directory, 1);

	module = p11_kit_module_load (BUILDDIR "/.libs/p11-kit-client" SHLEXT, 0);
	assert (module != NULL);

	rv = p11_kit_module_initialize (module);
	assert (rv == CKR_OK);

	rv = p11_kit_module_finalize (module);
	assert (rv == CKR_OK);

	p11_kit_module_release (module);
}

static void
test_open_session (void *unused)
{
	CK_SESSION_HANDLE session;
	CK_FUNCTION_LIST_PTR module;
	CK_SLOT_ID slots[32];
	CK_ULONG count;
	CK_RV rv;

	module = p11_kit_module_load (BUILDDIR "/.libs/p11-kit-client" SHLEXT, 0);
	assert (module != NULL);

	rv = p11_kit_module_initialize (module);
	assert (rv == CKR_OK);

	count = 32;
	rv = module->C_GetSlotList (CK_TRUE, slots, &count);
	assert (rv == CKR_OK);
	assert_num_eq (1, count);

	rv = module->C_OpenSession (slots[0], CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &session);
	assert (rv == CKR_OK);

	rv = module->C_CloseSession (session);
	assert (rv == CKR_OK);

	rv = p11_kit_module_finalize (module);
	assert (rv == CKR_OK);

	p11_kit_module_release (module);
}

static void
test_open_session_write_protected (void *unused)
{
	CK_SESSION_HANDLE session;
	CK_FUNCTION_LIST_PTR module;
	CK_SLOT_ID slots[32];
	CK_ULONG count;
	CK_RV rv;

	module = p11_kit_module_load (BUILDDIR "/.libs/p11-kit-client" SHLEXT, 0);
	assert (module != NULL);

	rv = p11_kit_module_initialize (module);
	assert (rv == CKR_OK);

	count = 32;
	rv = module->C_GetSlotList (CK_TRUE, slots, &count);
	assert (rv == CKR_OK);
	assert_num_eq (1, count);

	rv = module->C_OpenSession (slots[0], CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &session);
	assert (rv == CKR_TOKEN_WRITE_PROTECTED);

	rv = p11_kit_module_finalize (module);
	assert (rv == CKR_OK);

	p11_kit_module_release (module);
}

int
main (int argc,
      char *argv[])
{
	p11_library_init ();
	mock_module_init ();

	p11_fixture (setup_server, teardown_server);
	p11_testx (test_initialize, (void *)"pkcs11:", "/server/initialize");
	p11_testx (test_initialize_no_address, (void *)"pkcs11:", "/server/initialize-no-address");
	p11_testx (test_open_session, (void *)"pkcs11:", "/server/open-session");
	p11_testx (test_open_session_write_protected, (void *)"pkcs11:?write-protected=yes", "/server/open-session-write-protected");

	return p11_test_run (argc, argv);
}
