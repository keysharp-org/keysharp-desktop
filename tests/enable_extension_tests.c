/* The parts of enable-extension that can be decided without a desktop.
 *
 * The command itself needs a session bus, a shell and an installed GSettings
 * schema, none of which exist in CI. Its decisions do not: which uuid is in a
 * list, whether a directory appears in a search path, which credentials are
 * refused, and which status means the user must act. Those are separated out
 * precisely so they can be checked here rather than only on a live desktop.
 */
#include "enable_extension.h"

#include <assert.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void check_list_membership(void)
{
    const char *const two[] = { "a@b", "keysharp@keysharp.io", NULL };
    const char *const none[] = { NULL };

    assert(ksd_strv_contains(two, "keysharp@keysharp.io"));
    assert(!ksd_strv_contains(two, "keysharp@keysharp.i"));
    assert(!ksd_strv_contains(two, "eysharp@keysharp.io"));
    assert(!ksd_strv_contains(none, "keysharp@keysharp.io"));
    assert(!ksd_strv_contains(NULL, "keysharp@keysharp.io"));
    assert(!ksd_strv_contains(two, NULL));
}

static void check_addition_is_idempotent(void)
{
    const char *const empty[] = { NULL };
    const char *const present[] = { "keysharp@keysharp.io", NULL };
    const char *const other[] = { "a@b", NULL };
    char **result;

    result = ksd_strv_with(empty, "keysharp@keysharp.io");
    assert(g_strv_length(result) == 1u);
    assert(strcmp(result[0], "keysharp@keysharp.io") == 0);
    g_strfreev(result);

    /* Running the command twice must not list the uuid twice. A shell that
     * reads a duplicated entry is not guaranteed to be forgiving, and the
     * installers can and will run this more than once. */
    result = ksd_strv_with(present, "keysharp@keysharp.io");
    assert(g_strv_length(result) == 1u);
    g_strfreev(result);

    /* Everything already enabled stays enabled. */
    result = ksd_strv_with(other, "keysharp@keysharp.io");
    assert(g_strv_length(result) == 2u);
    assert(strcmp(result[0], "a@b") == 0);
    assert(strcmp(result[1], "keysharp@keysharp.io") == 0);
    g_strfreev(result);
}

static void check_removal_keeps_everything_else(void)
{
    const char *const three[] = { "a@b", "keysharp@keysharp.io", "c@d", NULL };
    char **result = ksd_strv_without(three, "keysharp@keysharp.io");

    assert(g_strv_length(result) == 2u);
    assert(strcmp(result[0], "a@b") == 0);
    assert(strcmp(result[1], "c@d") == 0);
    g_strfreev(result);

    /* Removing something absent is not an error and changes nothing. */
    result = ksd_strv_without(three, "nothing@here");
    assert(g_strv_length(result) == 3u);
    g_strfreev(result);
}

/* The search-path question that decides whether an enable can possibly work.
 *
 * This project's default prefix is /usr/local, and both shells resolve
 * extension directories through XDG_DATA_DIRS. An unset value means GLib
 * applies its own /usr/local/share:/usr/share default, under which /usr/local
 * IS searched -- but a session that sets the variable explicitly can leave it
 * out, and then the extension is installed somewhere the shell never looks.
 * That pair is the whole point of the check. */
static void check_search_path_matching(void)
{
    assert(ksd_data_dir_listed("/usr/local/share:/usr/share",
                               "/usr/local/share"));
    assert(!ksd_data_dir_listed("/usr/share:/usr/share/gnome",
                                "/usr/local/share"));

    /* A trailing slash on either side names the same directory. */
    assert(ksd_data_dir_listed("/usr/local/share/:/usr/share",
                               "/usr/local/share"));
    assert(ksd_data_dir_listed("/usr/local/share:/usr/share",
                               "/usr/local/share/"));

    /* Whole entries only: a prefix of a longer entry is not a match, or
     * /usr/share would appear to satisfy /usr/share/gnome and the reverse. */
    assert(!ksd_data_dir_listed("/usr/local/share/extra", "/usr/local/share"));
    assert(!ksd_data_dir_listed("/usr/share", "/usr"));

    /* Last entry, first entry, and empty elements. */
    assert(ksd_data_dir_listed("/a:/b:/usr/local/share", "/usr/local/share"));
    assert(ksd_data_dir_listed("/usr/local/share:/a", "/usr/local/share"));
    assert(ksd_data_dir_listed("::/usr/local/share:", "/usr/local/share"));
    assert(!ksd_data_dir_listed("", "/usr/local/share"));
    assert(!ksd_data_dir_listed(NULL, "/usr/local/share"));
    assert(!ksd_data_dir_listed("/usr/local/share", NULL));
}

/* Enablement is per-user dconf. Run as root it writes root's settings and
 * reports success, which is the original bug wearing a disguise, so the
 * refusal is as load-bearing as anything the command does. */
static void check_credentials_refused(void)
{
    assert(ksd_enable_credentials_refused(0u, 0u, 0u, 0u));
    /* Real user, effective root: a setuid binary partway through something. */
    assert(ksd_enable_credentials_refused(1000u, 0u, 1000u, 1000u));
    assert(ksd_enable_credentials_refused(0u, 1000u, 0u, 0u));
    /* Mismatched groups, same reasoning. */
    assert(ksd_enable_credentials_refused(1000u, 1000u, 1000u, 27u));
    /* A whole, unprivileged identity is the only accepted shape. */
    assert(!ksd_enable_credentials_refused(1000u, 1000u, 1000u, 1000u));
    assert(!ksd_enable_credentials_refused(1u, 1u, 1u, 1u));
}

/* The installers key on these. 0 must mean "nothing for the user to do",
 * including the KWin and X11 cases, or an installer would report a failure on
 * a machine that never needed an extension. */
static void check_exit_code_contract(void)
{
    assert(ksd_enable_exit_code(KSD_ENABLE_ALREADY_LIVE) == 0);
    assert(ksd_enable_exit_code(KSD_ENABLE_ENABLED) == 0);
    assert(ksd_enable_exit_code(KSD_ENABLE_NOT_APPLICABLE) == 0);

    assert(ksd_enable_exit_code(KSD_ENABLE_NEEDS_RELOGIN) == 3);
    assert(ksd_enable_exit_code(KSD_ENABLE_ALREADY_LISTED) == 3);
    assert(ksd_enable_exit_code(KSD_ENABLE_KILL_SWITCH) == 3);
    assert(ksd_enable_exit_code(KSD_ENABLE_SHELL_REJECTED) == 3);

    assert(ksd_enable_exit_code(KSD_ENABLE_NO_SHELL) == 1);
    assert(ksd_enable_exit_code(KSD_ENABLE_NO_BUS) == 1);
    assert(ksd_enable_exit_code(KSD_ENABLE_LOCKED) == 1);
    assert(ksd_enable_exit_code(KSD_ENABLE_NO_SCHEMA) == 1);
    assert(ksd_enable_exit_code(KSD_ENABLE_REFUSED) == 1);
}

/* Every status reports a name of its own. Two statuses sharing one name would
 * make the outcomes indistinguishable in exactly the output the installers and
 * the user read. */
static void check_status_names_are_distinct(void)
{
    static const ksd_enable_status all[] = {
        KSD_ENABLE_ALREADY_LIVE, KSD_ENABLE_ENABLED, KSD_ENABLE_NEEDS_RELOGIN,
        KSD_ENABLE_ALREADY_LISTED, KSD_ENABLE_NOT_APPLICABLE,
        KSD_ENABLE_NO_SHELL, KSD_ENABLE_NO_BUS, KSD_ENABLE_KILL_SWITCH,
        KSD_ENABLE_LOCKED, KSD_ENABLE_NO_SCHEMA, KSD_ENABLE_SHELL_REJECTED,
        KSD_ENABLE_REFUSED,
    };
    size_t count = sizeof(all) / sizeof(all[0]);

    for (size_t outer = 0u; outer < count; outer++) {
        const char *name = ksd_enable_status_name(all[outer]);
        assert(name != NULL && name[0] != '\0');
        assert(strcmp(name, "unknown") != 0);
        for (size_t inner = outer + 1u; inner < count; inner++)
            assert(strcmp(name, ksd_enable_status_name(all[inner])) != 0);
    }
}

typedef struct {
    ksd_enable_status status;
    unsigned *calls;
} activation_scenario;

static ksd_enable_status simulate_activation(const void *context)
{
    const activation_scenario *scenario = context;
    (*scenario->calls)++;
    return scenario->status;
}

static void check_session_activation(void)
{
    char *state = g_dir_make_tmp("keysharp-extension-tests-XXXXXX", NULL);
    assert(state != NULL);
    char *directory = g_build_filename(state, "keysharp-desktop", NULL);
    char *gnome = g_build_filename(directory, "extension-enabled-gnome", NULL);
    char *cinnamon = g_build_filename(directory,
                                      "extension-enabled-cinnamon", NULL);
    unsigned calls = 0u;
    activation_scenario scenario = { KSD_ENABLE_NO_SHELL, &calls };

    assert(ksd_enable_extension_once(state, "gnome", simulate_activation,
                                    &scenario) == KSD_ENABLE_NO_SHELL);
    assert(calls == 1u && !g_file_test(gnome, G_FILE_TEST_EXISTS));
    scenario.status = KSD_ENABLE_ENABLED;
    assert(ksd_enable_extension_once(state, "gnome", simulate_activation,
                                    &scenario) == KSD_ENABLE_ENABLED);
    assert(calls == 2u && g_file_test(gnome, G_FILE_TEST_EXISTS));

    /* A later manual disable must not call the activator again. */
    scenario.status = KSD_ENABLE_KILL_SWITCH;
    assert(ksd_enable_extension_once(state, "gnome", simulate_activation,
                                    &scenario) == KSD_ENABLE_NOT_APPLICABLE);
    assert(calls == 2u);

    scenario.status = KSD_ENABLE_ALREADY_LIVE;
    assert(ksd_enable_extension_once(state, "cinnamon", simulate_activation,
                                    &scenario) == KSD_ENABLE_ALREADY_LIVE);
    assert(calls == 3u && g_file_test(cinnamon, G_FILE_TEST_EXISTS));
    assert(g_remove(gnome) == 0 && g_remove(cinnamon) == 0);

    const ksd_enable_status completed[] = {
        KSD_ENABLE_ALREADY_LIVE, KSD_ENABLE_ENABLED,
        KSD_ENABLE_NEEDS_RELOGIN, KSD_ENABLE_ALREADY_LISTED,
    };
    for (size_t index = 0u; index < G_N_ELEMENTS(completed); index++) {
        scenario.status = completed[index];
        assert(ksd_enable_extension_once(state, "gnome", simulate_activation,
                                        &scenario) == completed[index]);
        assert(g_file_test(gnome, G_FILE_TEST_EXISTS));
        assert(g_remove(gnome) == 0);
    }

    const ksd_enable_status incomplete[] = {
        KSD_ENABLE_NOT_APPLICABLE, KSD_ENABLE_NO_SHELL, KSD_ENABLE_NO_BUS,
        KSD_ENABLE_KILL_SWITCH, KSD_ENABLE_LOCKED, KSD_ENABLE_NO_SCHEMA,
        KSD_ENABLE_SHELL_REJECTED, KSD_ENABLE_REFUSED,
    };
    for (size_t index = 0u; index < G_N_ELEMENTS(incomplete); index++) {
        scenario.status = incomplete[index];
        assert(ksd_enable_extension_once(state, "gnome", simulate_activation,
                                        &scenario) == incomplete[index]);
        assert(!g_file_test(gnome, G_FILE_TEST_EXISTS));
    }
    unsigned prior = calls;
    assert(ksd_enable_extension_once(state, "other", simulate_activation,
                                    &scenario) == KSD_ENABLE_NOT_APPLICABLE);
    assert(calls == prior);

    assert(g_rmdir(directory) == 0 && g_rmdir(state) == 0);
    g_free(cinnamon);
    g_free(gnome);
    g_free(directory);
    g_free(state);
}

int main(void)
{
    check_list_membership();
    check_addition_is_idempotent();
    check_removal_keeps_everything_else();
    check_search_path_matching();
    check_credentials_refused();
    check_exit_code_contract();
    check_status_names_are_distinct();
    check_session_activation();
    return 0;
}
