/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"
#include "datatype.h"
#include "dataloop.h"
#include "mpir_typerep.h"

static void update_type_vector(int count, int blocklength, MPI_Aint stride, MPI_Datatype oldtype,
                               MPIR_Datatype * newtype, int strideinbytes)
{
    int old_is_contig;
    MPI_Aint old_sz;
    MPI_Aint old_lb, old_ub, old_extent, old_true_lb, old_true_ub, eff_stride;

    if (HANDLE_IS_BUILTIN(oldtype)) {
        MPI_Aint el_sz = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);

        old_lb = 0;
        old_true_lb = 0;
        old_ub = el_sz;
        old_true_ub = el_sz;
        old_sz = el_sz;
        old_extent = el_sz;
        old_is_contig = 1;

        newtype->size = (MPI_Aint) count *(MPI_Aint) blocklength *el_sz;

        newtype->alignsize = el_sz;     /* ??? */
        newtype->n_builtin_elements = count * blocklength;
        newtype->builtin_element_size = el_sz;
        newtype->basic_type = oldtype;

        eff_stride = (strideinbytes) ? stride : (stride * el_sz);
    } else {    /* user-defined base type (oldtype) */

        MPIR_Datatype *old_dtp;

        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        old_lb = old_dtp->lb;
        old_true_lb = old_dtp->true_lb;
        old_ub = old_dtp->ub;
        old_true_ub = old_dtp->true_ub;
        old_sz = old_dtp->size;
        old_extent = old_dtp->extent;
        MPIR_Datatype_is_contig(oldtype, &old_is_contig);

        newtype->size = count * blocklength * old_dtp->size;

        newtype->alignsize = old_dtp->alignsize;
        newtype->n_builtin_elements = count * blocklength * old_dtp->n_builtin_elements;
        newtype->builtin_element_size = old_dtp->builtin_element_size;
        newtype->basic_type = old_dtp->basic_type;

        eff_stride = (strideinbytes) ? stride : (stride * old_dtp->extent);
    }

    MPII_DATATYPE_VECTOR_LB_UB((MPI_Aint) count,
                               eff_stride,
                               (MPI_Aint) blocklength,
                               old_lb, old_ub, old_extent, newtype->lb, newtype->ub);
    newtype->true_lb = newtype->lb + (old_true_lb - old_lb);
    newtype->true_ub = newtype->ub + (old_true_ub - old_ub);
    newtype->extent = newtype->ub - newtype->lb;

    /* new type is only contig for N types if old one was, and
     * size and extent of new type are equivalent, and stride is
     * equal to blocklength * size of old type.
     */
    if ((MPI_Aint) (newtype->size) == newtype->extent &&
        eff_stride == (MPI_Aint) blocklength * old_sz && old_is_contig) {
        newtype->is_contig = 1;
    } else {
        newtype->is_contig = 0;
    }
}

static void update_type_indexed(int count, const int *blocklength_array,
                                const void *displacement_array, MPI_Datatype oldtype,
                                MPIR_Datatype * newtype, int dispinbytes)
{
    int old_is_contig;
    int i;
    MPI_Aint contig_count;
    MPI_Aint el_ct, old_ct, old_sz;
    MPI_Aint old_lb, old_ub, old_extent, old_true_lb, old_true_ub;
    MPI_Aint min_lb = 0, max_ub = 0, eff_disp;

    if (HANDLE_IS_BUILTIN(oldtype)) {
        /* builtins are handled differently than user-defined types because
         * they have no associated typerep or datatype structure.
         */
        MPI_Aint el_sz = MPIR_Datatype_get_basic_size(oldtype);
        old_sz = el_sz;
        el_ct = 1;

        old_lb = 0;
        old_true_lb = 0;
        old_ub = (MPI_Aint) el_sz;
        old_true_ub = (MPI_Aint) el_sz;
        old_extent = (MPI_Aint) el_sz;
        old_is_contig = 1;

        MPIR_Assign_trunc(newtype->alignsize, el_sz, MPI_Aint);
        newtype->builtin_element_size = el_sz;
        newtype->basic_type = oldtype;
    } else {
        /* user-defined base type (oldtype) */
        MPIR_Datatype *old_dtp;

        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        /* Ensure that "builtin_element_size" fits into an int datatype. */
        MPIR_Ensure_Aint_fits_in_int(old_dtp->builtin_element_size);

        old_sz = old_dtp->size;
        el_ct = old_dtp->n_builtin_elements;

        old_lb = old_dtp->lb;
        old_true_lb = old_dtp->true_lb;
        old_ub = old_dtp->ub;
        old_true_ub = old_dtp->true_ub;
        old_extent = old_dtp->extent;
        MPIR_Datatype_is_contig(oldtype, &old_is_contig);

        newtype->alignsize = old_dtp->alignsize;
        newtype->builtin_element_size = old_dtp->builtin_element_size;
        newtype->basic_type = old_dtp->basic_type;
    }

    /* find the first nonzero blocklength element */
    i = 0;
    while (i < count && blocklength_array[i] == 0)
        i++;

    MPIR_Assert(i < count);

    /* priming for loop */
    old_ct = blocklength_array[i];
    eff_disp = (dispinbytes) ? ((MPI_Aint *) displacement_array)[i] :
        (((MPI_Aint) ((int *) displacement_array)[i]) * old_extent);

    MPII_DATATYPE_BLOCK_LB_UB((MPI_Aint) blocklength_array[i],
                              eff_disp, old_lb, old_ub, old_extent, min_lb, max_ub);

    /* determine min lb, max ub, and count of old types in remaining
     * nonzero size blocks
     */
    for (i++; i < count; i++) {
        MPI_Aint tmp_lb, tmp_ub;

        if (blocklength_array[i] > 0) {
            old_ct += blocklength_array[i];     /* add more oldtypes */

            eff_disp = (dispinbytes) ? ((MPI_Aint *) displacement_array)[i] :
                (((MPI_Aint) ((int *) displacement_array)[i]) * old_extent);

            /* calculate ub and lb for this block */
            MPII_DATATYPE_BLOCK_LB_UB((MPI_Aint) (blocklength_array[i]),
                                      eff_disp, old_lb, old_ub, old_extent, tmp_lb, tmp_ub);

            if (tmp_lb < min_lb)
                min_lb = tmp_lb;
            if (tmp_ub > max_ub)
                max_ub = tmp_ub;
        }
    }

    newtype->size = old_ct * old_sz;

    newtype->lb = min_lb;
    newtype->ub = max_ub;
    newtype->true_lb = min_lb + (old_true_lb - old_lb);
    newtype->true_ub = max_ub + (old_true_ub - old_ub);
    newtype->extent = max_ub - min_lb;

    newtype->n_builtin_elements = old_ct * el_ct;

    /* new type is only contig for N types if it's all one big
     * block, its size and extent are the same, and the old type
     * was also contiguous.
     */
    newtype->is_contig = 0;
    if (old_is_contig) {
        MPI_Aint *blklens = MPL_malloc(count * sizeof(MPI_Aint), MPL_MEM_DATATYPE);
        MPIR_Assert(blklens != NULL);
        for (i = 0; i < count; i++)
            blklens[i] = blocklength_array[i];
        contig_count = MPII_Datatype_indexed_count_contig(count,
                                                          blklens,
                                                          displacement_array, dispinbytes,
                                                          old_extent);
        if ((contig_count == 1) && ((MPI_Aint) newtype->size == newtype->extent)) {
            newtype->is_contig = 1;
        }
        MPL_free(blklens);
    }

}

static void update_type_blockindexed(int count, int blocklength, const void *displacement_array,
                                     MPI_Datatype oldtype, MPIR_Datatype * newtype, int dispinbytes)
{
    int i;
    int old_is_contig;
    MPI_Aint contig_count;
    MPI_Aint old_lb, old_ub, old_extent, old_true_lb, old_true_ub;
    MPI_Aint min_lb = 0, max_ub = 0, eff_disp;

    if (HANDLE_IS_BUILTIN(oldtype)) {
        MPI_Aint el_sz = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);

        old_lb = 0;
        old_true_lb = 0;
        old_ub = el_sz;
        old_true_ub = el_sz;
        old_extent = el_sz;
        old_is_contig = 1;

        newtype->size = (MPI_Aint) count *(MPI_Aint) blocklength *el_sz;

        newtype->alignsize = el_sz;     /* ??? */
        newtype->n_builtin_elements = count * blocklength;
        newtype->builtin_element_size = el_sz;
        newtype->basic_type = oldtype;
    } else {
        /* user-defined base type (oldtype) */
        MPIR_Datatype *old_dtp;

        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        old_lb = old_dtp->lb;
        old_true_lb = old_dtp->true_lb;
        old_ub = old_dtp->ub;
        old_true_ub = old_dtp->true_ub;
        old_extent = old_dtp->extent;
        MPIR_Datatype_is_contig(oldtype, &old_is_contig);

        newtype->size = (MPI_Aint) count *(MPI_Aint) blocklength *(MPI_Aint) old_dtp->size;

        newtype->alignsize = old_dtp->alignsize;
        newtype->n_builtin_elements = count * blocklength * old_dtp->n_builtin_elements;
        newtype->builtin_element_size = old_dtp->builtin_element_size;
        newtype->basic_type = old_dtp->basic_type;
    }

    /* priming for loop */
    eff_disp = (dispinbytes) ? ((MPI_Aint *) displacement_array)[0] :
        (((MPI_Aint) ((int *) displacement_array)[0]) * old_extent);
    MPII_DATATYPE_BLOCK_LB_UB((MPI_Aint) blocklength,
                              eff_disp, old_lb, old_ub, old_extent, min_lb, max_ub);

    /* determine new min lb and max ub */
    for (i = 1; i < count; i++) {
        MPI_Aint tmp_lb, tmp_ub;

        eff_disp = (dispinbytes) ? ((MPI_Aint *) displacement_array)[i] :
            (((MPI_Aint) ((int *) displacement_array)[i]) * old_extent);
        MPII_DATATYPE_BLOCK_LB_UB((MPI_Aint) blocklength,
                                  eff_disp, old_lb, old_ub, old_extent, tmp_lb, tmp_ub);

        if (tmp_lb < min_lb)
            min_lb = tmp_lb;
        if (tmp_ub > max_ub)
            max_ub = tmp_ub;
    }

    newtype->lb = min_lb;
    newtype->ub = max_ub;
    newtype->true_lb = min_lb + (old_true_lb - old_lb);
    newtype->true_ub = max_ub + (old_true_ub - old_ub);
    newtype->extent = max_ub - min_lb;

    /* new type is contig for N types if it is all one big block,
     * its size and extent are the same, and the old type was also
     * contiguous.
     */
    newtype->is_contig = 0;
    if (old_is_contig) {
        contig_count = MPII_Datatype_blockindexed_count_contig(count,
                                                               blocklength,
                                                               displacement_array,
                                                               dispinbytes, old_extent);
        if ((contig_count == 1) && ((MPI_Aint) newtype->size == newtype->extent)) {
            newtype->is_contig = 1;
        }
    }
}

/* end static functions */

int MPIR_Typerep_create_vector(int count, int blocklength, int stride, MPI_Datatype oldtype,
                               MPIR_Datatype * newtype)
{
    int old_is_contig;
    MPI_Aint old_extent;

    update_type_vector(count, blocklength, stride, oldtype, newtype, 0);

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = count;
        old_is_contig = 1;
        old_extent = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);
        newtype->typerep.num_contig_blocks =
            old_dtp->typerep.num_contig_blocks * count * blocklength;

        MPIR_Datatype_is_contig(oldtype, &old_is_contig);
        old_extent = old_dtp->extent;
    }

    if (old_is_contig && stride * old_extent == old_extent * blocklength) {
        newtype->typerep.num_contig_blocks = 1;
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_hvector(int count, int blocklength, MPI_Aint stride, MPI_Datatype oldtype,
                                MPIR_Datatype * newtype)
{
    int old_is_contig;
    MPI_Aint old_extent;

    update_type_vector(count, blocklength, stride, oldtype, newtype, 1);

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = count;
        old_is_contig = 1;
        old_extent = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);
        newtype->typerep.num_contig_blocks =
            old_dtp->typerep.num_contig_blocks * count * blocklength;

        MPIR_Datatype_is_contig(oldtype, &old_is_contig);
        old_extent = old_dtp->extent;
    }

    if (old_is_contig && stride == old_extent * blocklength) {
        newtype->typerep.num_contig_blocks = 1;
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_contig(int count, MPI_Datatype oldtype, MPIR_Datatype * newtype)
{
    if (HANDLE_IS_BUILTIN(oldtype)) {
        MPI_Aint el_sz = MPIR_Datatype_get_basic_size(oldtype);

        newtype->size = count * el_sz;
        newtype->true_lb = 0;
        newtype->lb = 0;
        newtype->true_ub = count * el_sz;
        newtype->ub = newtype->true_ub;
        newtype->extent = newtype->ub - newtype->lb;

        newtype->alignsize = el_sz;
        newtype->n_builtin_elements = count;
        newtype->builtin_element_size = el_sz;
        newtype->basic_type = oldtype;
        newtype->is_contig = 1;
    } else {
        /* user-defined base type (oldtype) */
        MPIR_Datatype *old_dtp;

        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        newtype->size = count * old_dtp->size;

        MPII_DATATYPE_CONTIG_LB_UB((MPI_Aint) count,
                                   old_dtp->lb,
                                   old_dtp->ub, old_dtp->extent, newtype->lb, newtype->ub);

        /* easiest to calc true lb/ub relative to lb/ub; doesn't matter
         * if there are sticky lb/ubs or not when doing this.
         */
        newtype->true_lb = newtype->lb + (old_dtp->true_lb - old_dtp->lb);
        newtype->true_ub = newtype->ub + (old_dtp->true_ub - old_dtp->ub);
        newtype->extent = newtype->ub - newtype->lb;

        newtype->alignsize = old_dtp->alignsize;
        newtype->n_builtin_elements = count * old_dtp->n_builtin_elements;
        newtype->builtin_element_size = old_dtp->builtin_element_size;
        newtype->basic_type = old_dtp->basic_type;

        MPIR_Datatype_is_contig(oldtype, &newtype->is_contig);
    }

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = 1;
    } else if (newtype->is_contig) {
        newtype->typerep.num_contig_blocks = 1;
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);
        newtype->typerep.num_contig_blocks = count * old_dtp->typerep.num_contig_blocks;
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_dup(MPI_Datatype oldtype, MPIR_Datatype * newtype)
{
    MPIR_Datatype *dtp;

    MPIR_Datatype_get_ptr(oldtype, dtp);
    if (dtp->is_committed)
        MPIR_Dataloop_dup(dtp->typerep.handle, &newtype->typerep.handle);

    newtype->is_contig = dtp->is_contig;
    newtype->size = dtp->size;
    newtype->extent = dtp->extent;
    newtype->ub = dtp->ub;
    newtype->lb = dtp->lb;
    newtype->true_ub = dtp->true_ub;
    newtype->true_lb = dtp->true_lb;
    newtype->alignsize = dtp->alignsize;

    newtype->n_builtin_elements = dtp->n_builtin_elements;
    newtype->builtin_element_size = dtp->builtin_element_size;
    newtype->basic_type = dtp->basic_type;

    newtype->typerep.num_contig_blocks = dtp->typerep.num_contig_blocks;

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_indexed_block(int count, int blocklength, const int *array_of_displacements,
                                      MPI_Datatype oldtype, MPIR_Datatype * newtype)
{
    int old_is_contig;
    MPI_Aint old_extent;

    update_type_blockindexed(count, blocklength, array_of_displacements, oldtype, newtype, 0);

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = count;
        old_is_contig = 1;
        old_extent = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);
        newtype->typerep.num_contig_blocks =
            count * old_dtp->typerep.num_contig_blocks * blocklength;

        MPIR_Datatype_is_contig(oldtype, &old_is_contig);
        old_extent = old_dtp->extent;
    }

    if (old_is_contig) {
        newtype->typerep.num_contig_blocks =
            MPII_Datatype_blockindexed_count_contig(count, blocklength,
                                                    (const void *) array_of_displacements,
                                                    0, old_extent);
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_hindexed_block(int count, int blocklength,
                                       const MPI_Aint * array_of_displacements,
                                       MPI_Datatype oldtype, MPIR_Datatype * newtype)
{
    int old_is_contig;
    MPI_Aint old_extent;

    update_type_blockindexed(count, blocklength, array_of_displacements, oldtype, newtype, 1);

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = count;
        old_is_contig = 1;
        old_extent = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);
        newtype->typerep.num_contig_blocks =
            count * old_dtp->typerep.num_contig_blocks * blocklength;

        MPIR_Datatype_is_contig(oldtype, &old_is_contig);
        old_extent = old_dtp->extent;
    }

    if (old_is_contig) {
        newtype->typerep.num_contig_blocks =
            MPII_Datatype_blockindexed_count_contig(count, blocklength,
                                                    (const void *) array_of_displacements,
                                                    1, old_extent);
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_indexed(int count, const int *array_of_blocklengths,
                                const int *array_of_displacements, MPI_Datatype oldtype,
                                MPIR_Datatype * newtype)
{
    int old_is_contig;
    MPI_Aint old_extent;

    update_type_indexed(count, array_of_blocklengths, array_of_displacements, oldtype, newtype, 0);

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = count;
        old_is_contig = 1;
        old_extent = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        newtype->typerep.num_contig_blocks = 0;
        for (int i = 0; i < count; i++)
            newtype->typerep.num_contig_blocks +=
                old_dtp->typerep.num_contig_blocks * array_of_blocklengths[i];

        MPIR_Datatype_is_contig(oldtype, &old_is_contig);
        old_extent = old_dtp->extent;
    }

    if (old_is_contig) {
        MPI_Aint *blklens = MPL_malloc(count * sizeof(MPI_Aint), MPL_MEM_DATATYPE);
        MPIR_Assert(blklens != NULL);
        for (int i = 0; i < count; i++)
            blklens[i] = (MPI_Aint) array_of_blocklengths[i];
        newtype->typerep.num_contig_blocks =
            MPII_Datatype_indexed_count_contig(count, blklens,
                                               (const void *) array_of_displacements, 0,
                                               old_extent);
        MPL_free(blklens);
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_hindexed(int count, const int *array_of_blocklengths,
                                 const MPI_Aint * array_of_displacements, MPI_Datatype oldtype,
                                 MPIR_Datatype * newtype)
{
    int old_is_contig;
    MPI_Aint old_extent;

    update_type_indexed(count, array_of_blocklengths, array_of_displacements, oldtype, newtype, 1);

    if (HANDLE_IS_BUILTIN(oldtype)) {
        newtype->typerep.num_contig_blocks = count;
        old_is_contig = 1;
        old_extent = (MPI_Aint) MPIR_Datatype_get_basic_size(oldtype);
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        newtype->typerep.num_contig_blocks = 0;
        for (int i = 0; i < count; i++)
            newtype->typerep.num_contig_blocks +=
                old_dtp->typerep.num_contig_blocks * array_of_blocklengths[i];

        MPIR_Datatype_is_contig(oldtype, &old_is_contig);
        old_extent = old_dtp->extent;
    }

    if (old_is_contig) {
        MPI_Aint *blklens = MPL_malloc(count * sizeof(MPI_Aint), MPL_MEM_DATATYPE);
        MPIR_Assert(blklens != NULL);
        for (int i = 0; i < count; i++)
            blklens[i] = (MPI_Aint) array_of_blocklengths[i];
        newtype->typerep.num_contig_blocks =
            MPII_Datatype_indexed_count_contig(count, blklens,
                                               (const void *) array_of_displacements, 1,
                                               old_extent);
        MPL_free(blklens);
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_resized(MPI_Datatype oldtype, MPI_Aint lb, MPI_Aint extent,
                                MPIR_Datatype * newtype)
{
    if (HANDLE_IS_BUILTIN(oldtype)) {
        int oldsize = MPIR_Datatype_get_basic_size(oldtype);

        newtype->size = oldsize;
        newtype->true_lb = 0;
        newtype->lb = lb;
        newtype->true_ub = oldsize;
        newtype->ub = lb + extent;
        newtype->extent = extent;
        newtype->alignsize = oldsize;   /* FIXME ??? */
        newtype->n_builtin_elements = 1;
        newtype->builtin_element_size = oldsize;
        newtype->is_contig = (extent == oldsize) ? 1 : 0;
        newtype->basic_type = oldtype;
        newtype->typerep.num_contig_blocks = 3; /* lb, data, ub */
    } else {
        MPIR_Datatype *old_dtp;
        MPIR_Datatype_get_ptr(oldtype, old_dtp);

        newtype->size = old_dtp->size;
        newtype->true_lb = old_dtp->true_lb;
        newtype->lb = lb;
        newtype->true_ub = old_dtp->true_ub;
        newtype->ub = lb + extent;
        newtype->extent = extent;
        newtype->alignsize = old_dtp->alignsize;
        newtype->n_builtin_elements = old_dtp->n_builtin_elements;
        newtype->builtin_element_size = old_dtp->builtin_element_size;
        newtype->basic_type = old_dtp->basic_type;

        if (extent == old_dtp->size)
            MPIR_Datatype_is_contig(oldtype, &newtype->is_contig);
        else
            newtype->is_contig = 0;
        newtype->typerep.num_contig_blocks = old_dtp->typerep.num_contig_blocks;
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_struct(int count, const int *array_of_blocklengths,
                               const MPI_Aint * array_of_displacements,
                               const MPI_Datatype * array_of_types, MPIR_Datatype * newtype)
{
    newtype->typerep.num_contig_blocks = 0;
    for (int i = 0; i < count; i++) {
        if (HANDLE_IS_BUILTIN(array_of_types[i])) {
            newtype->typerep.num_contig_blocks++;
        } else {
            MPIR_Datatype *old_dtp;
            MPIR_Datatype_get_ptr(array_of_types[i], old_dtp);
            newtype->typerep.num_contig_blocks +=
                old_dtp->typerep.num_contig_blocks * array_of_blocklengths[i];
        }
    }

    return MPI_SUCCESS;
}

int MPIR_Typerep_create_subarray(int ndims, const int *array_of_sizes, const int *array_of_subsizes,
                                 const int *array_of_starts, int order,
                                 MPI_Datatype oldtype, MPIR_Datatype * newtype)
{
    return MPI_SUCCESS;
}

int MPIR_Typerep_create_darray(int size, int rank, int ndims, const int *array_of_gsizes,
                               const int *array_of_distribs, const int *array_of_dargs,
                               const int *array_of_psizes, int order, MPI_Datatype oldtype,
                               MPIR_Datatype * newtype)
{
    return MPI_SUCCESS;
}
