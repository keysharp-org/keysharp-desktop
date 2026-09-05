#include <gio/gio.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_source(const char *root, const char *relative)
{
    char path[4096];
    char *contents = NULL;
    GError *error = NULL;

    if (g_snprintf(path, sizeof path, "%s/%s", root, relative) < 0)
        return NULL;
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        fprintf(stderr, "cannot read %s: %s\n", path,
                error != NULL ? error->message : "unknown");
        g_clear_error(&error);
        return NULL;
    }
    return contents;
}

static char *extract_literal(const char *source, const char *name)
{
    const char *at = strstr(source, name);
    const char *open = NULL;
    const char *close = NULL;

    if (at == NULL)
        return NULL;
    open = strchr(at, '`');
    if (open == NULL)
        return NULL;
    close = strchr(open + 1, '`');
    if (close == NULL)
        return NULL;
    return g_strndup(open + 1, (gsize)(close - open - 1));
}

/* A standalone interface file, rather than a literal inside a provider script.
 * The KWin interface is owned by the daemon and called BY the script, so it has
 * no JavaScript to be embedded in -- but it is the same kind of thing and needs
 * the same check, because GLib refuses a whole interface over one malformed
 * comment and the failure surfaces as a channel that never comes up. */
static void check_interface_file(const char *provider, const char *relative,
                                 const char *root, const char *required,
                                 size_t methods)
{
    char *xml = read_source(root, relative);
    GDBusNodeInfo *info = NULL;
    GError *error = NULL;
    GDBusInterfaceInfo *iface;
    size_t count = 0u;

    assert(xml != NULL);
    info = g_dbus_node_info_new_for_xml(xml, &error);
    if (info == NULL) {
        fprintf(stderr, "%s: %s is not valid D-Bus XML: %s\n",
                provider, relative, error != NULL ? error->message : "unknown");
        g_clear_error(&error);
        abort();
    }
    assert(info->interfaces != NULL && info->interfaces[0] != NULL);
    iface = info->interfaces[0];
    assert(g_dbus_interface_info_lookup_method(iface, required) != NULL);

    /* Every method carries the SAME signature, and the count is pinned. That
     * discipline is what keeps the channel alive: callDBus drops its callback
     * on any error including a signature mismatch, so a slip here would not
     * fail loudly, it would make the script go deaf with jobs in flight. */
    while (iface->methods != NULL && iface->methods[count] != NULL) {
        GDBusMethodInfo *method = iface->methods[count];

        assert(method->in_args != NULL);
        assert(method->in_args[0] != NULL
               && g_strcmp0(method->in_args[0]->signature, "s") == 0);
        assert(method->in_args[1] != NULL
               && g_strcmp0(method->in_args[1]->signature, "s") == 0);
        assert(method->in_args[2] == NULL);
        assert(method->out_args != NULL && method->out_args[0] != NULL
               && g_strcmp0(method->out_args[0]->signature, "s") == 0);
        assert(method->out_args[1] == NULL);
        count++;
    }
    assert(count == methods);

    printf("  %-9s %-17s ok (%s, %zu methods)\n", provider, "interface file",
           iface->name, count);
    g_dbus_node_info_unref(info);
    g_free(xml);
}

static void check_interface(const char *provider, const char *relative,
                            const char *name, const char *root,
                            const char *required)
{
    char *source = read_source(root, relative);
    char *xml = NULL;
    GDBusNodeInfo *info = NULL;
    GError *error = NULL;

    assert(source != NULL);
    xml = extract_literal(source, name);
    if (xml == NULL) {
        fprintf(stderr, "%s: %s literal not found\n", provider, name);
        abort();
    }

    const char *scan = xml;
    while ((scan = strstr(scan, "<!--")) != NULL) {
        const char *end = strstr(scan, "-->");
        const char *hit = NULL;
        if (end == NULL)
            break;
        for (hit = scan + 4; hit + 1 < end; hit++) {
            if (hit[0] == '-' && hit[1] == '-') {
                fprintf(stderr, "%s: %s has '--' inside an XML comment\n",
                        provider, name);
                fprintf(stderr,
                        "  illegal per the XML spec; GLib tolerates it but a"
                        " stricter parser need not\n");
                abort();
            }
        }
        scan = end + 3;
    }

    info = g_dbus_node_info_new_for_xml(xml, &error);
    if (info == NULL) {
        fprintf(stderr, "%s: %s is not valid D-Bus XML: %s\n",
                provider, name, error != NULL ? error->message : "unknown");
        fprintf(stderr,
                "  a '--' sequence inside an XML comment is the usual cause;"
                " it is illegal in XML and gnome-shell refuses the whole"
                " interface, which fails extension enable()\n");
        g_clear_error(&error);
        abort();
    }
    assert(info->interfaces != NULL && info->interfaces[0] != NULL);

    if (required != NULL) {
        GDBusInterfaceInfo *iface = info->interfaces[0];
        assert(g_dbus_interface_info_lookup_method(iface, required) != NULL);
    }

    printf("  %-9s %-17s ok (%s)\n", provider, name,
           info->interfaces[0]->name);
    g_dbus_node_info_unref(info);
    g_free(xml);
    g_free(source);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : ".";

    check_interface_file("kwin",
                         "interfaces/private/io.github.keysharp.KWinProvider1.xml",
                         root, "Poll", 4u);
    check_interface("gnome", "providers/gnome/extension.js",
                    "DBUS_IFACE_XML", root, "QueryWindow");
    check_interface("gnome", "providers/gnome/extension.js",
                    "PUBLIC_IFACE_XML", root, NULL);
    check_interface("cinnamon", "providers/cinnamon/extension.js",
                    "DBUS_IFACE_XML", root, "QueryWindow");
    check_interface("cinnamon", "providers/cinnamon/extension.js",
                    "PUBLIC_IFACE_XML", root, NULL);
    return 0;
}
