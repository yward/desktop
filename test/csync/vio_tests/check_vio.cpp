/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008-2013 by Andreas Schneider <asn@cryptomilk.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "csync_private.h"
#include "std/c_utf8.h"
#include "vio/csync_vio.h"

#include "torture.h"

#define CSYNC_TEST_DIR "/tmp/csync_test/"
#define CSYNC_TEST_DIRS "/tmp/csync_test/this/is/a/mkdirs/test"
#define CSYNC_TEST_FILE "/tmp/csync_test/file.txt"

#define MKDIR_MASK (S_IRWXU |S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)

#define WD_BUFFER_SIZE 255

static char wd_buffer[WD_BUFFER_SIZE];

static int setup(void **state)
{
    CSYNC *csync = nullptr;
    int rc = 0;

    assert_non_null(getcwd(wd_buffer, WD_BUFFER_SIZE));

    rc = system("rm -rf /tmp/csync_test");
    assert_int_equal(rc, 0);

    csync = new CSYNC("/tmp/check_csync1", new OCC::SyncJournalDb(""));

    csync->current = LOCAL_REPLICA;

    *state = csync;
    return 0;
}

static int setup_dir(void **state) {
    int rc = 0;
    mbchar_t *dir = c_utf8_path_to_locale(CSYNC_TEST_DIR);

    setup(state);

    rc = _tmkdir(dir, MKDIR_MASK);
    c_free_locale_string(dir);
    assert_int_equal(rc, 0);

    assert_non_null(getcwd(wd_buffer, WD_BUFFER_SIZE));

    rc = chdir(CSYNC_TEST_DIR);
    assert_int_equal(rc, 0);
    return 0;
}

static int teardown(void **state) {
    CSYNC *csync = (CSYNC*)*state;
    int rc = 0;

    auto statedb = csync->statedb;
    delete csync;
    delete statedb;

    rc = chdir(wd_buffer);
    assert_int_equal(rc, 0);

    rc = system("rm -rf /tmp/csync_test/");
    assert_int_equal(rc, 0);

    *state = NULL;
    return 0;
}


/*
 * Test directory function
 */

static void check_csync_vio_opendir(void **state)
{
    CSYNC *csync = (CSYNC*)*state;
    csync_vio_handle_t *dh = nullptr;
    int rc = 0;

    dh = csync_vio_opendir(csync, CSYNC_TEST_DIR);
    assert_non_null(dh);

    rc = csync_vio_closedir(csync, dh);
    assert_int_equal(rc, 0);
}

static void check_csync_vio_opendir_perm(void **state)
{
    CSYNC *csync = (CSYNC*)*state;
    csync_vio_handle_t *dh = nullptr;
    int rc = 0;
    mbchar_t *dir = c_utf8_path_to_locale(CSYNC_TEST_DIR);

    assert_non_null(dir);

    rc = _tmkdir(dir, (S_IWUSR|S_IXUSR));
    assert_int_equal(rc, 0);

    dh = csync_vio_opendir(csync, CSYNC_TEST_DIR);
    assert_null(dh);
    assert_int_equal(errno, EACCES);

    _tchmod(dir, MKDIR_MASK);
    c_free_locale_string(dir);
}

static void check_csync_vio_closedir_null(void **state)
{
    CSYNC *csync = (CSYNC*)*state;
    int rc = 0;

    rc = csync_vio_closedir(csync, NULL);
    assert_int_equal(rc, -1);
}

int torture_run_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(check_csync_vio_opendir, setup_dir, teardown),
        cmocka_unit_test_setup_teardown(check_csync_vio_opendir_perm, setup, teardown),
        cmocka_unit_test(check_csync_vio_closedir_null),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
