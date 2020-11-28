/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "sd-login.h"

#include "bus-map-properties.h"
#include "hostname-util.h"
#include "locale-util.h"
#include "memory-util.h"
#include "sort-util.h"
#include "systemctl-list-machines.h"
#include "systemctl-util.h"
#include "systemctl.h"
#include "terminal-util.h"

const struct bus_properties_map machine_info_property_map[] = {
        { "SystemState",        "s", NULL, offsetof(struct machine_info, state)          },
        { "NJobs",              "u", NULL, offsetof(struct machine_info, n_jobs)         },
        { "NFailedUnits",       "u", NULL, offsetof(struct machine_info, n_failed_units) },
        { "ControlGroup",       "s", NULL, offsetof(struct machine_info, control_group)  },
        { "UserspaceTimestamp", "t", NULL, offsetof(struct machine_info, timestamp)      },
        {}
};

void machine_info_clear(struct machine_info *info) {
        assert(info);

        free(info->name);
        free(info->state);
        free(info->control_group);
        zero(*info);
}

static void free_machines_list(struct machine_info *machine_infos, int n) {
        int i;

        if (!machine_infos)
                return;

        for (i = 0; i < n; i++)
                machine_info_clear(&machine_infos[i]);

        free(machine_infos);
}

static int compare_machine_info(const struct machine_info *a, const struct machine_info *b) {
        int r;

        r = CMP(b->is_host, a->is_host);
        if (r != 0)
                return r;

        return strcasecmp(a->name, b->name);
}

static int get_machine_properties(sd_bus *bus, struct machine_info *mi) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *container = NULL;
        int r;

        assert(mi);

        if (!bus) {
                r = sd_bus_open_system_machine(&container, mi->name);
                if (r < 0)
                        return r;

                bus = container;
        }

        r = bus_map_all_properties(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        machine_info_property_map,
                        BUS_MAP_STRDUP,
                        NULL,
                        NULL,
                        mi);
        if (r < 0)
                return r;

        return 0;
}

static bool output_show_machine(const char *name, char **patterns) {
        return strv_fnmatch_or_empty(patterns, name, FNM_NOESCAPE);
}

static int get_machine_list(
                sd_bus *bus,
                struct machine_info **_machine_infos,
                char **patterns) {

        struct machine_info *machine_infos = NULL;
        _cleanup_strv_free_ char **m = NULL;
        _cleanup_free_ char *hn = NULL;
        size_t sz = 0;
        char **i;
        int c = 0, r;

        hn = gethostname_malloc();
        if (!hn)
                return log_oom();

        if (output_show_machine(hn, patterns)) {
                if (!GREEDY_REALLOC0(machine_infos, sz, c+1))
                        return log_oom();

                machine_infos[c].is_host = true;
                machine_infos[c].name = TAKE_PTR(hn);

                (void) get_machine_properties(bus, &machine_infos[c]);
                c++;
        }

        r = sd_get_machine_names(&m);
        if (r < 0)
                return log_error_errno(r, "Failed to get machine list: %m");

        STRV_FOREACH(i, m) {
                _cleanup_free_ char *class = NULL;

                if (!output_show_machine(*i, patterns))
                        continue;

                sd_machine_get_class(*i, &class);
                if (!streq_ptr(class, "container"))
                        continue;

                if (!GREEDY_REALLOC0(machine_infos, sz, c+1)) {
                        free_machines_list(machine_infos, c);
                        return log_oom();
                }

                machine_infos[c].is_host = false;
                machine_infos[c].name = strdup(*i);
                if (!machine_infos[c].name) {
                        free_machines_list(machine_infos, c);
                        return log_oom();
                }

                (void) get_machine_properties(NULL, &machine_infos[c]);
                c++;
        }

        *_machine_infos = machine_infos;
        return c;
}

static int output_machines_list(struct machine_info *machine_infos, unsigned n) {
        _cleanup_(table_unrefp) Table *table = NULL;
        struct machine_info *m;
        bool state_missing = false;
        int r;

        assert(machine_infos || n == 0);

        table = table_new("", "name", "state", "failed", "jobs");
        if (!table)
                return log_oom();

        table_set_header(table, !arg_no_legend);
        if (arg_plain) {
                /* Hide the 'glyph' column when --plain is requested */
                r = table_hide_column_from_display(table, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to hide column: %m");
        }
        if (arg_full)
                table_set_width(table, 0);

        (void) table_set_empty_string(table, "-");

        for (m = machine_infos; m < machine_infos + n; m++) {
                _cleanup_free_ char *mname = NULL;
                const char *on_state = "", *on_failed = "";
                bool circle = false;

                if (streq_ptr(m->state, "degraded")) {
                        on_state = ansi_highlight_red();
                        circle = true;
                } else if (!streq_ptr(m->state, "running")) {
                        on_state = ansi_highlight_yellow();
                        circle = true;
                }

                if (m->n_failed_units > 0)
                        on_failed = ansi_highlight_red();
                else
                        on_failed =  "";

                if (!m->state)
                        state_missing = true;

                if (m->is_host)
                        mname = strjoin(strna(m->name), " (host)");

                r = table_add_many(table,
                                   TABLE_STRING, circle ? special_glyph(SPECIAL_GLYPH_BLACK_CIRCLE) : " ",
                                   TABLE_SET_COLOR, on_state,
                                   TABLE_STRING, m->is_host ? mname : strna(m->name),
                                   TABLE_STRING, strna(m->state),
                                   TABLE_SET_COLOR, on_state,
                                   TABLE_UINT32, m->n_failed_units,
                                   TABLE_SET_COLOR, on_failed,
                                   TABLE_UINT32, m->n_jobs);
                if (r < 0)
                        return table_log_add_error(r);
        }

        r = output_table(table);
        if (r < 0)
                return r;

        if (!arg_no_legend) {
                printf("\n");
                if (state_missing && geteuid() != 0)
                        printf("Notice: some information only available to privileged users was not shown.\n");
                printf("%u machines listed.\n", n);
        }

        return 0;
}

int list_machines(int argc, char *argv[], void *userdata) {
        struct machine_info *machine_infos = NULL;
        sd_bus *bus;
        int r, rc;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = get_machine_list(bus, &machine_infos, strv_skip(argv, 1));
        if (r < 0)
                return r;

        (void) pager_open(arg_pager_flags);

        typesafe_qsort(machine_infos, r, compare_machine_info);
        rc = output_machines_list(machine_infos, r);
        free_machines_list(machine_infos, r);

        return rc;
}