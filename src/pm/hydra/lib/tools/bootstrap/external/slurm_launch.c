/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "hydra.h"
#include "bsci.h"
#include "bscu.h"
#include "topo.h"
#include "slurm.h"

static int fd_stdout, fd_stderr;

static HYD_status proxy_list_to_node_str(struct HYD_proxy *proxy_list, char **node_list_str)
{
    int i;
    char *tmp[HYD_NUM_TMP_STRINGS], *foo = NULL;
    struct HYD_proxy *proxy;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    i = 0;
    for (proxy = proxy_list; proxy; proxy = proxy->next) {
        tmp[i++] = MPL_strdup(proxy->node->hostname);

        if (proxy->node->next)
            tmp[i++] = MPL_strdup(",");

        /* If we used up more than half of the array elements, merge
         * what we have so far */
        if (i > (HYD_NUM_TMP_STRINGS / 2)) {
            tmp[i++] = NULL;
            status = HYDU_str_alloc_and_join(tmp, &foo);
            HYDU_ERR_POP(status, "error joining strings\n");

            i = 0;
            tmp[i++] = MPL_strdup(foo);
            MPL_free(foo);
        }
    }

    tmp[i++] = NULL;
    status = HYDU_str_alloc_and_join(tmp, &foo);
    HYDU_ERR_POP(status, "error joining strings\n");

    *node_list_str = foo;
    foo = NULL;

  fn_exit:
    HYDU_free_strlist(tmp);
    MPL_free(foo);
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

HYD_status HYDT_bscd_slurm_launch_procs(char **args, struct HYD_proxy *proxy_list, int use_rmk,
                                        int *control_fd)
{
    int num_hosts, idx, i;
    int *fd_list;
    char *targs[HYD_NUM_TMP_STRINGS], *node_list_str = NULL;
    char *path = NULL, *extra_arg_list = NULL, *extra_arg;
    struct HYD_proxy *proxy;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    /* We use the following priority order for the executable path:
     * (1) user-specified; (2) search in path; (3) Hard-coded
     * location */
    if (HYDT_bsci_info.launcher_exec)
        path = MPL_strdup(HYDT_bsci_info.launcher_exec);
    if (!path)
        path = HYDU_find_full_path("srun");
    if (!path)
        path = MPL_strdup("/usr/bin/srun");
    if (!path)
        HYDU_ERR_POP(status, "error allocating memory for strdup\n");

    idx = 0;
    targs[idx++] = MPL_strdup(path);

    if (use_rmk == HYD_FALSE || strcmp(HYDT_bsci_info.rmk, "slurm")) {
        targs[idx++] = MPL_strdup("--nodelist");

        status = proxy_list_to_node_str(proxy_list, &node_list_str);
        HYDU_ERR_POP(status, "unable to build a node list string\n");

        targs[idx++] = MPL_strdup(node_list_str);
    }

    num_hosts = 0;
    for (proxy = proxy_list; proxy; proxy = proxy->next)
        num_hosts++;

    targs[idx++] = MPL_strdup("-N");
    targs[idx++] = HYDU_int_to_str(num_hosts);

    targs[idx++] = MPL_strdup("-n");
    targs[idx++] = HYDU_int_to_str(num_hosts);

    /* Force srun to ignore stdin to avoid issues with
     * unexpected files open on fd 0 */
    targs[idx++] = MPL_strdup("--input");
    targs[idx++] = MPL_strdup("none");

    if (MPL_env2str("HYDRA_LAUNCHER_EXTRA_ARGS", (const char **) &extra_arg_list)) {
        extra_arg = strtok(extra_arg_list, " ");
        while (extra_arg) {
            targs[idx++] = MPL_strdup(extra_arg);
            extra_arg = strtok(NULL, " ");
        }
    }

    /* Fill in the remaining arguments */
    /* We do not need to create a quoted version of the string for
     * SLURM. It seems to be internally quoting it anyway. */
    for (i = 0; args[i]; i++)
        targs[idx++] = MPL_strdup(args[i]);

    /* Increase pid list to accommodate the new pid */
    HYDT_bscu_pid_list_grow(1);

    /* Increase fd list to accommodate these new fds */
    HYDU_MALLOC_OR_JUMP(fd_list, int *, (HYD_bscu_fd_count + 3) * sizeof(int), status);
    for (i = 0; i < HYD_bscu_fd_count; i++)
        fd_list[i] = HYD_bscu_fd_list[i];
    MPL_free(HYD_bscu_fd_list);
    HYD_bscu_fd_list = fd_list;

    /* append proxy ID as -1 */
    targs[idx++] = HYDU_int_to_str(-1);
    targs[idx++] = NULL;

    if (HYDT_bsci_info.debug) {
        HYDU_dump(stdout, "Launch arguments: ");
        HYDU_print_strlist(targs);
    }

    int pid;
    status = HYDU_create_process(targs, NULL, NULL, &fd_stdout, &fd_stderr, &pid, -1);
    HYDU_ERR_POP(status, "create process returned error\n");
    HYDT_bscu_pid_list_push(NULL, pid);

    HYD_bscu_fd_list[HYD_bscu_fd_count++] = fd_stdout;
    HYD_bscu_fd_list[HYD_bscu_fd_count++] = fd_stderr;

    status = HYDT_dmx_register_fd(1, &fd_stdout, HYD_POLLIN,
                                  (void *) (size_t) STDOUT_FILENO, HYDT_bscu_stdio_cb);
    HYDU_ERR_POP(status, "demux returned error registering fd\n");

    status = HYDT_dmx_register_fd(1, &fd_stderr, HYD_POLLIN,
                                  (void *) (size_t) STDERR_FILENO, HYDT_bscu_stdio_cb);
    HYDU_ERR_POP(status, "demux returned error registering fd\n");

  fn_exit:
    MPL_free(node_list_str);
    HYDU_free_strlist(targs);
    MPL_free(path);
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}
