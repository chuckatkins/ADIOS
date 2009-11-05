#include <stdlib.h>
#include <string.h>
#include "adios.h"
#include "bp_utils.h"
#include "bp_types.h"
#include "common_read.h"
#include "adios_read.h"
#include "adios_error.h"
#include "futils.h"
#define BYTE_ALIGN 8

#ifdef DMALLOC
#include "dmalloc.h"
#endif


/* Return 0: if file is little endian, 1 if file is big endian 
 * We know if it is different from the current system, so here
 * we determine the current endianness and report accordingly.
 */
static int common_read_get_endianness( uint32_t change_endianness )
{
   int LE = 0;
   int BE = !LE;
   int i = 1;
   char *p = (char *) &i;
   int current_endianness;
   if (p[0] == 1) // Lowest address contains the least significant byte
       current_endianness = LE;
   else
       current_endianness = BE;
    if (change_endianness == adios_flag_yes)
        return !current_endianness;
    else
        return current_endianness;
}

ADIOS_FILE * common_read_fopen (const char * fname, MPI_Comm comm)
{
    int i, rank;    
    struct BP_FILE * fh;
    ADIOS_FILE * fp;
    uint64_t header_size;

    adios_errno = 0;
    fh = (struct BP_FILE *) malloc (sizeof (struct BP_FILE));
    if (!fh) {
        error( err_no_memory, "Cannot allocate memory for file info.");
        return NULL;
    }
    fh->comm =  comm;
    fh->gvar_h = 0;
    fh->pgs_root = 0;
    fh->vars_root = 0;
    fh->attrs_root = 0;
    fh->b = malloc (sizeof (struct adios_bp_buffer_struct_v1));
    if (!fh->b) {
        error( err_no_memory, "Cannot allocate memory for file info.");
        return NULL;
    }
    fp = (ADIOS_FILE *) malloc (sizeof (ADIOS_FILE));
    if (!fp) {
        error( err_no_memory, "Cannot allocate memory for file info.");
        return NULL;
    }

    adios_buffer_struct_init (fh->b);
    MPI_Comm_rank (comm, &rank);
    if (bp_read_open (fname, comm, fh))
        return NULL;

    /* Only rank=0 process reads the footer and it broadcasts to all other processes */
    if ( rank == 0 ) {
        if (bp_read_minifooter (fh))
            return NULL;
    }
    MPI_Bcast (&fh->mfooter, sizeof(struct bp_minifooter),MPI_BYTE, 0, comm);
    
    header_size = fh->mfooter.file_size-fh->mfooter.pgs_index_offset;

    if ( rank != 0) {
        if (!fh->b->buff) {
            bp_alloc_aligned (fh->b, header_size);
            if (!fh->b->buff)
                return NULL;
            memset (fh->b->buff, 0, header_size);
            fh->b->offset = 0;
        }
    }
    MPI_Barrier(comm);
    MPI_Bcast (fh->b->buff, fh->mfooter.file_size-fh->mfooter.pgs_index_offset, MPI_BYTE, 0 , comm);

    /* Everyone parses the index on its own */
    bp_parse_pgs (fh);
    bp_parse_vars (fh);
    bp_parse_attrs (fh);

    /* fill out ADIOS_FILE struct */
    fp->fh = (uint64_t) fh;
    fp->groups_count = fh->gvar_h->group_count;
    fp->vars_count = fh->mfooter.vars_count;
    fp->attrs_count = fh->mfooter.attrs_count;
    fp->tidx_start = fh->tidx_start;
    fp->ntimesteps = fh->tidx_stop - fh->tidx_start + 1;
    fp->file_size = fh->mfooter.file_size;
    fp->version = fh->mfooter.version;
    fp->endianness = common_read_get_endianness(fh->mfooter.change_endianness);
    alloc_namelist (&fp->group_namelist,fp->groups_count); 
    for (i=0;i<fp->groups_count;i++) {
        if (!fp->group_namelist[i]) {
            error(err_no_memory, "Could not allocate buffer for %d strings in adios_fopen()", fp->groups_count);
            common_read_fclose(fp);
            return NULL;
        }
        else  {
            strcpy(fp->group_namelist[i],fh->gvar_h->namelist[i]);
        }
    }
    return fp;
}

int common_read_fclose (ADIOS_FILE *fp) 
{
    struct BP_FILE * fh = (struct BP_FILE *) fp->fh;
    struct BP_GROUP_VAR * gh = fh->gvar_h;
    struct BP_GROUP_ATTR * ah = fh->gattr_h;
    struct adios_index_var_struct_v1 * vars_root = fh->vars_root, *vr;
    struct adios_index_attribute_struct_v1 * attrs_root = fh->attrs_root, *ar;
    struct bp_index_pg_struct_v1 * pgs_root = fh->pgs_root, *pr;
    int i,j;
    MPI_File mpi_fh = fh->mpi_fh;

    adios_errno = 0;
    if (fh->mpi_fh) 
        MPI_File_close (&mpi_fh);
    if (fh->b) {
        adios_posix_close_internal (fh->b);
        free(fh->b);
    }

    /* Free variable structures */
    /* alloc in bp_utils.c: bp_parse_vars() */
    while (vars_root) {
        vr = vars_root;
        vars_root = vars_root->next;
        for (j = 0; j < vr->characteristics_count; j++) {
            // alloc in bp_utils.c:bp_parse_characteristics() <- bp_get_characteristics_data()
            if (vr->characteristics[j].dims.dims)
                free (vr->characteristics[j].dims.dims);
            if (vr->characteristics[j].value)
                free (vr->characteristics[j].value);
            if (vr->characteristics[j].min)
                free (vr->characteristics[j].min);
            if (vr->characteristics[j].max)
                free (vr->characteristics[j].max);
        }
        if (vr->characteristics) 
            free (vr->characteristics);
        if (vr->group_name) 
            free (vr->group_name);
        if (vr->var_name) 
            free (vr->var_name);
        if (vr->var_path) 
            free (vr->var_path);
        free(vr);
    }

    /* Free attributes structures */
    /* alloc in bp_utils.c bp_parse_attrs() */
    while (attrs_root) {
        ar = attrs_root;
        attrs_root = attrs_root->next;
        for (j = 0; j < ar->characteristics_count; j++) {
            if (ar->characteristics[j].value)
                free (ar->characteristics[j].value);
        }
        if (ar->characteristics) 
            free (ar->characteristics);
        if (ar->group_name) 
            free (ar->group_name);
        if (ar->attr_name) 
            free (ar->attr_name);
        if (ar->attr_path) 
            free (ar->attr_path);
        free(ar);
    }


    /* Free process group structures */
    /* alloc in bp_utils.c bp_parse_pgs() first loop */
    //printf ("pgs: %d\n", fh->mfooter.pgs_count);
    while (pgs_root) {
        pr = pgs_root;
        pgs_root = pgs_root->next;
        //printf("%d\tpg pid=%d addr=%x next=%x\n",i, pr->process_id, pr, pr->next);
        if (pr->group_name)
            free(pr->group_name);
        if (pr->time_index_name)
            free(pr->time_index_name);
        free(pr);
    }

    /* Free variable structures in BP_GROUP_VAR */
    if (gh) {
        for (j=0;j<2;j++) { 
            for (i=0;i<gh->group_count;i++) {
                if (gh->time_index[j][i])
                    free(gh->time_index[j][i]);
            }
            if (gh->time_index[j])
                free(gh->time_index[j]);
        }
        free (gh->time_index);
    
        for (i=0;i<gh->group_count;i++) { 
            if (gh->namelist[i])
                free(gh->namelist[i]);
        }
        if (gh->namelist)
            free (gh->namelist);

        for (i=0;i<fh->mfooter.vars_count;i++) {
            if (gh->var_namelist[i])
                free(gh->var_namelist[i]);
            if (gh->var_offsets[i]) 
                free(gh->var_offsets[i]);
        }
        if (gh->var_namelist)
            free (gh->var_namelist);

        if (gh->var_offsets) 
            free(gh->var_offsets);

        if (gh->var_counts_per_group)
            free(gh->var_counts_per_group);

        if (gh->pg_offsets)
            free (gh->pg_offsets);

        free (gh);
    }

    /* Free attribute structures in BP_GROUP_ATTR */
    if (ah) {
        for (i = 0; i < fh->mfooter.attrs_count; i++) {
            if (ah->attr_offsets[i]) 
                free(ah->attr_offsets[i]);
            if (ah->attr_namelist[i]) 
                free(ah->attr_namelist[i]);
        }
        if (ah->attr_offsets)
            free(ah->attr_offsets);
        if (ah->attr_namelist)
            free(ah->attr_namelist);
        if (ah->attr_counts_per_group) 
            free(ah->attr_counts_per_group);

        free(ah);
    }
        
    if (fh)
        free (fh);    

    free_namelist ((fp->group_namelist),fp->groups_count);
    free(fp);
    return 0;
}


ADIOS_GROUP * common_read_gopen (ADIOS_FILE *fp, const char * grpname)
{
    struct BP_FILE * fh = (struct BP_FILE *) fp->fh;
    int grpid; 

    adios_errno = 0;
    for (grpid=0;grpid<(fh->gvar_h->group_count);grpid++) {
        if (!strcmp(fh->gvar_h->namelist[grpid], grpname))
            break; 
    }
    if (grpid >= fh->gvar_h->group_count) {
        error( err_invalid_group, "Invalid group name %s", grpname);
        return NULL;
    }
    return common_read_gopen_byid(fp, grpid);
}

ADIOS_GROUP * common_read_gopen_byid (ADIOS_FILE *fp, int grpid)
{
    struct BP_FILE * fh = (struct BP_FILE *) fp->fh;
    struct BP_GROUP * gh;
    ADIOS_GROUP * gp;
    int i, offset;

    adios_errno = 0;
    if (grpid < 0 || grpid >= fh->gvar_h->group_count) {
        error( err_invalid_group, "Invalid group index %d", grpid);
        return NULL;
    }

    gh = (struct BP_GROUP *) malloc(sizeof(struct BP_GROUP));
    if (!gh) {
        error( err_no_memory, "Could not allocate memory for group info");
        return NULL;
    }

    gp = (ADIOS_GROUP *) malloc(sizeof(ADIOS_GROUP));
    if (!gp) {
        error( err_no_memory, "Could not allocate memory for group info");
        return NULL;
    }

    /* set offset index of variables (which is a long list of all vars in all groups) in this group */
    offset = 0;
    for (i=0; i<grpid; i++)
        offset += fh->gvar_h->var_counts_per_group[i];

    /* gh->vars_root will point to the list of vars in this group */
    gh->vars_root = fh->vars_root; 
    for (i=0; i<offset; i++)
        gh->vars_root = gh->vars_root->next;

    gh->group_id = grpid;
    gh->vars_offset = offset;
    gh->vars_count = fh->gvar_h->var_counts_per_group[grpid];

    /* set offset of attributes in this group */
    offset = 0;
    for(i=0;i<grpid;i++)
        offset += fh->gattr_h->attr_counts_per_group[i];

    /* gh->attrs_root will point to the list of vars in this group */
    gh->attrs_root = fh->attrs_root; 
    for (i=0; i<offset; i++)
        gh->attrs_root = gh->attrs_root->next;

    gh->attrs_offset = offset;
    gh->attrs_count = fh->gattr_h->attr_counts_per_group[grpid];

    gh->fh = fh; 

    /* fill out ADIOS_GROUP struct */
    gp->grpid = grpid;
    gp->gh = (uint64_t) gh;
    gp->fp = fp;
    gp->vars_count = gh->vars_count;
    gp->attrs_count = gh->attrs_count;
    
    offset = gh->vars_offset;
    alloc_namelist (&(gp->var_namelist), gp->vars_count);
    for (i=0;i<gp->vars_count;i++) {
        if (!gp->var_namelist[i]) { 
            error(err_no_memory, "Could not allocate buffer for %d strings in adios_gopen()", gp->vars_count);
            common_read_gclose(gp);
            return NULL;
        }
        else
            strcpy(gp->var_namelist[i], gh->fh->gvar_h->var_namelist[i+offset]);
    }

    offset = gh->attrs_offset;
    alloc_namelist (&(gp->attr_namelist), gp->attrs_count);
    for (i=0;i<gp->attrs_count;i++) {
        if (!gp->attr_namelist[i]) {
            error(err_no_memory, "Could not allocate buffer for %d strings in adios_gopen()", gp->vars_count);
            common_read_gclose(gp);
            return NULL;
        }
        else {
            strcpy(gp->attr_namelist[i], gh->fh->gattr_h->attr_namelist[i+offset]);
        }
    }

    return gp;
}
                   
int common_read_gclose (ADIOS_GROUP *gp)
{
    struct BP_GROUP * gh = (struct BP_GROUP *) gp->gh;

    adios_errno = 0;
    if (!gh) {
        error (err_invalid_group_struct, "group handle is NULL!");
        return  err_invalid_group_struct;
    }
    else
        free (gh);

    free_namelist ((gp->var_namelist),gp->vars_count);
    free_namelist ((gp->attr_namelist),gp->attrs_count);
    free(gp);
    return 0;
}



int common_read_get_attr (ADIOS_GROUP * gp, const char * attrname, enum ADIOS_DATATYPES * type,
                    int * size, void ** data)
{
    // Find the attribute: full path is stored with a starting / 
    // Like in HDF5, we need to match names given with or without the starting /
    // startpos is 0 or 1 to indicate if the argument has starting / or not
    int attrid;
    int vstartpos = 0, fstartpos = 0; 
    struct BP_GROUP * gh = (struct BP_GROUP *)gp->gh;
    int offset;

    adios_errno = 0;
    if (!gp) {
        error(err_invalid_group_struct, "Null pointer passed as group to adios_get_attr()");
        return adios_errno;
    }
    if (!attrname) {
        error(err_invalid_attrname, "Null pointer passed as attribute name to adios_get_attr()!");
        return adios_errno;
    }

    offset = gh->attrs_offset;

    if (attrname[0] == '/') 
        vstartpos = 1;
    for (attrid=0; attrid<(gp->attrs_count);attrid++) {
        //if (gp->attr_namelist[attrid][0] == '/') 
        if (gh->fh->gattr_h->attr_namelist[attrid+offset][0] == '/') 
            fstartpos = 1;
        //if (!strcmp(gp->attr_namelist[attrid]+fstartpos, attrname+vstartpos))
        if (!strcmp(gh->fh->gattr_h->attr_namelist[attrid+offset]+fstartpos, attrname+vstartpos))
            break; 
    }
    if (attrid >= gp->attrs_count) {
        error( err_invalid_attrname, "Invalid attribute name %s", attrname);
        return adios_errno;
    }

    return common_read_get_attr_byid(gp, attrid, type, size, data);
}

int common_read_get_attr_byid (ADIOS_GROUP * gp, int attrid, 
                    enum ADIOS_DATATYPES * type, int * size, void ** data)
{
    int    i, offset, count;
    struct BP_GROUP * gh;
    struct BP_FILE * fh;
    struct adios_index_attribute_struct_v1 * attr_root;
    struct adios_index_var_struct_v1 * var_root;
    int    file_is_fortran;

    adios_errno = 0;
    if (!gp) {
        error(err_invalid_group_struct, "Null pointer passed as group to adios_get_attr()");
        return adios_errno;
    }
    gh = (struct BP_GROUP *) gp->gh;
    if (!gh) {
        error(err_invalid_group_struct, "Invalid ADIOS_GROUP struct: .gh group handle is NULL!");
        return adios_errno;
    }
    fh = gh->fh;
    if (!fh) {
        error(err_invalid_group_struct, "Invalid ADIOS_GROUP struct: .gh->fh file handle is NULL!");
        return adios_errno;
    }
    if (attrid < 0 || attrid >= gh->attrs_count) {
        error(err_invalid_attrid, "Invalid attribute id %d (allowed 0..%d)", attrid, gh->attrs_count);
        return adios_errno;
    }

    attr_root = gh->attrs_root; /* need to traverse the attribute list of the group */
    for (i = 0; i < attrid && attr_root; i++)
        attr_root = attr_root->next;
    if (i != attrid) {
        error (err_corrupted_attribute, "Attribute id=%d is valid but was not found in internal data structures!",attrid);
        return adios_errno; 
    }

    file_is_fortran = (fh->pgs_root->adios_host_language_fortran == adios_flag_yes);

    if (attr_root->characteristics[0].value) {
        /* Attribute has its own value */
        *size = bp_get_type_size (attr_root->type, attr_root->characteristics[0].value);
        *type = attr_root->type;
        *data = (void *) malloc (*size);  
        if (*data)
            memcpy(*data, attr_root->characteristics[0].value, *size);
    }
    else if (attr_root->characteristics[0].var_id) {
        /* Attribute is a reference to a variable */
        /* FIXME: var ids are not unique in BP. If a group of variables are written several
           times under different path using adios_set_path(), the id of a variable is always
           the same (should be different). As a temporary fix, we look first for a matching
           id plus path between an attribute and a variable. If not found, then we look for
           a match on the ids only.*/
        var_root = gh->vars_root; 
        while (var_root) {
            if (var_root->id == attr_root->characteristics[0].var_id && 
                !strcmp(var_root->var_path, attr_root->attr_path))
                break;
            var_root = var_root->next;
        }
        if (!var_root) {
            var_root = gh->vars_root; 
            while (var_root) {
                if (var_root->id == attr_root->characteristics[0].var_id)
                    break;
                var_root = var_root->next;
            }
        }

        if (!var_root) {
            error (err_invalid_attribute_reference, 
                   "Attribute %s/%s in group %s is a reference to variable ID %d, which is not found", 
                   attr_root->attr_path, attr_root->attr_name, attr_root->group_name,
                   attr_root->characteristics[0].var_id);
            return adios_errno;
        }

        /* default values in case of error */
        *data = NULL;
        *size = 0;
        *type = attr_root->type;

        /* FIXME: variable and attribute type may not match, then a conversion is needed. */
        /* Cases:
                1. attr has no type, var is byte array     ==> string
                2. attr has no type, var is not byte array ==> var type
                3. attr is string, var is byte array       ==> string
                4. attr type == var type                   ==> var type 
                5. attr type != var type                   ==> attr type and conversion needed 
        */
        /* Error check: attr cannot reference an array in general */
        if (var_root->characteristics[0].dims.count > 0) {
            if ( (var_root->type == adios_byte || var_root->type == adios_unsigned_byte) &&
                 (attr_root->type == adios_unknown || attr_root->type == adios_string) &&
                 (var_root->characteristics[0].dims.count == 1)) {
                 ; // this conversions are allowed
            } else {
                error(err_invalid_attribute_reference, 
                    "Attribute %s/%s in group %s, typeid=%d is a reference to an %d-dimensional array variable "
                    "%s/%s of type %s, which is not supported in ADIOS",
                    attr_root->attr_path, attr_root->attr_name, attr_root->group_name, attr_root->type,
                    var_root->characteristics[0].dims.count,
                    var_root->var_path, var_root->var_name, common_read_type_to_string(var_root->type));
                return adios_errno;
            }
        }

        if ( (attr_root->type == adios_unknown || attr_root->type == adios_string) &&
             (var_root->type == adios_byte || var_root->type == adios_unsigned_byte) &&
             (var_root->characteristics[0].dims.count == 1) ) {
            /* 1D byte arrays are converted to string */
            /* 1. read in variable */
            char varname[512];
            char *tmpdata;
            uint64_t start, count;
            int status;
            start = 0; 
            count = var_root->characteristics[0].dims.dims[0];
            snprintf(varname, 512, "%s/%s", var_root->var_path, var_root->var_name);
            tmpdata = (char *) malloc (count+1);
            if (tmpdata == NULL) {
                error(err_no_memory, 
                      "Cannot allocate memory of %lld bytes for reading in data for attribute %s/%s of group %s.",
                      count, attr_root->attr_path, attr_root->attr_name, attr_root->group_name);
                return adios_errno;
            }

            status = common_read_read_var (gp, varname, &start, &count, tmpdata);
            
            if (status < 0) {
                char *msg = strdup(adios_get_last_errmsg());
                error((enum ADIOS_ERRCODES) status, 
                      "Cannot read data of variable %s/%s for attribute %s/%s of group %s: %s",
                      var_root->var_path, var_root->var_name, 
                      attr_root->attr_path, attr_root->attr_name, attr_root->group_name,
                      msg);
                free(tmpdata);
                free(msg);
                return status;
            }

            *type = adios_string;
            if (file_is_fortran) {
                /* Fortran byte array to C string */
                *data = futils_fstr_to_cstr( tmpdata, (int)count); /* FIXME: supports only 2GB strings... */
                *size = strlen( (char *)data );
                free(tmpdata);
            } else {
                /* C array to C string */
                tmpdata[count] = '\0';
                *size = count+1;
                *data = tmpdata;
            }
        } else {
            /* other types are inherited */
            *type = var_root->type;
            *size = bp_get_type_size (var_root->type, var_root->characteristics[0].value);
            *data = (void *) malloc (*size);  
            if (*data)
                memcpy(*data, var_root->characteristics[0].value, *size);
        }
    }

    return 0;
}


/* Reverse the order in an array in place.
   use swapping from Fortran/column-major order to ADIOS-read-api/C/row-major order and back
*/
static void swap_order(int n, uint64_t *array, int *timedim)
{
    int i;
    uint64_t tmp;
    for (i=0; i<n/2; i++) {
        tmp = array[i];
        array[i] = array[n-1-i];
        array[n-1-i] = tmp;
    }
    if (*timedim > -1)
        *timedim = (n-1) - *timedim; // swap the time dimension too
}

/* Look up variable id based on variable name.
   Return index 0..gp->vars_count-1 if found, -1 otherwise
*/
static int common_read_find_var(ADIOS_GROUP *gp, const char *varname)
{
    // Find the variable: full path is stored with a starting / 
    // Like in HDF5, we need to match names given with or without the starting /
    // startpos is 0 or 1 to indicate if the argument has starting / or not
    int varid;
    int vstartpos = 0, fstartpos = 0; 
    struct BP_GROUP * gh = (struct BP_GROUP *)gp->gh;
    int offset;

    adios_errno = 0;
    if (!gp) {
        error(err_invalid_group_struct, "Null pointer passed as group");
        return -1;
    }
    if (!varname) {
        error(err_invalid_varname, "Null pointer passed as variable name!");
        return -1;
    }

    /* Search in gp->fh->gvar_h->var_namelist instead of gp->var_namelist, because that
       one comes back from the user. One idiot (who writes this comment) sorted the
       list in place after gopen and before inq_var.
    */
    offset = gh->vars_offset;

    if (varname[0] == '/') 
        vstartpos = 1;
    for (varid=0; varid<(gp->vars_count);varid++) {
        /* if (gp->var_namelist[varid][0] == '/') */
        if (gh->fh->gvar_h->var_namelist[varid+offset][0] == '/')
            fstartpos = 1;
        /*if (!strcmp(gp->var_namelist[varid]+fstartpos, varname+vstartpos))*/
        if (!strcmp(gh->fh->gvar_h->var_namelist[varid+offset]+fstartpos, varname+vstartpos))
            break; 
    }
    if (varid >= gp->vars_count) {
        error(err_invalid_varname, "Invalid variable name %s", varname);
        return -1;
    }
    return varid;
}

/** Get value and min/max values, allocate space for them too */
static void common_read_get_characteristics (struct adios_index_var_struct_v1 * var_root, ADIOS_VARINFO *vi)
{
    int i;
    int size;

    /* set value for scalars */
    void *vval = var_root->characteristics [0].value;
    if (vval) {
        size = bp_get_type_size(var_root->type, vval);
        vi->value = (void *) malloc (size);
        if (vi->value)
           memcpy(vi->value, vval, size);
    } else {
        vi->value = NULL;
    }

    /* calculate min from min characteristics */
    void *vmin = NULL;
    for (i=0; i<var_root->characteristics_count; i++) {
        if (!vmin || adios_lt(var_root->type, var_root->characteristics[i].min, vmin))
           vmin = var_root->characteristics[i].min;
    }
    if (vmin) {
        size = bp_get_type_size(var_root->type, vmin);
        vi->gmin = (void *) malloc (size);
        if (vi->gmin)
           memcpy(vi->gmin, vmin, size);
    } else {
        vi->gmin = vi->value; // scalars have value but not min/max
    }

    if (!vval && vmin) {
        vi->value = vi->gmin; // arrays have no value but we assign here the minimum
    }

    /* calculate max from max characteristics */
    void *vmax = NULL;
    for (i=0; i<var_root->characteristics_count; i++) {
        if (!vmax || adios_lt(var_root->type, vmax, var_root->characteristics[i].max))
           vmax = var_root->characteristics[i].max;
    }
    if (vmax) {
        size = bp_get_type_size(var_root->type, vmax);
        vi->gmax = (void *) malloc (size);
        if (vi->gmax)
           memcpy(vi->gmax, vmax, size);
    } else {
        vi->gmax = vi->value; // scalars have value but not min/max
    }

    vi->mins = NULL;
    vi->maxs = NULL;
}

/* get local and global dimensions and offsets from a variable characteristics 
   return: 1 = it is a global array, 0 = local array
*/
static int common_read_get_dimensioncharacteristics(struct adios_index_characteristic_struct_v1 *ch, 
                                               uint64_t *ldims, uint64_t *gdims, uint64_t *offsets)
{
    int is_global=0; // global array or just an array written by one process?
    int ndim = ch->dims.count;
    int k;
    for (k=0; k < ndim; k++) {
        ldims[k]   = ch->dims.dims[k*3];
        gdims[k]   = ch->dims.dims[k*3+1];
        offsets[k] = ch->dims.dims[k*3+2];
        is_global = is_global || gdims[k];
    }
    return is_global;
}

static void common_read_get_dimensions (struct adios_index_var_struct_v1 *var_root, int ntsteps, int file_is_fortran,
                                  int *ndim, uint64_t **dims, int *timedim)
{
    int i, k;
    int is_global; // global array or just an array written by one process?
    uint64_t ldims[32];
    uint64_t gdims[32];
    uint64_t offsets[32];

    /* Get dimension information */    
    *ndim = var_root->characteristics [0].dims.count; 
    *dims = NULL;
    *timedim = -1;
    if (*ndim == 0) {
        /* done with this scalar variable */
        return ;
    }

    *dims = (uint64_t *) malloc (sizeof(uint64_t) * (*ndim));
    memset(*dims,0,sizeof(uint64_t)*(*ndim));

    is_global = common_read_get_dimensioncharacteristics( &(var_root->characteristics[0]),
                                                    ldims, gdims, offsets);

    if (!is_global) {
        /* local array */
        for (i=0; i < *ndim; i++) {
            /* size of time dimension is always registered as 1 for an array */
            if (ldims[i] == 1 && var_root->characteristics_count > 1) {
                *timedim = i;
                (*dims)[i] = ntsteps;
            } else {
                (*dims)[i] = ldims[i];
            }
            gdims[i] = ldims[i];
        }         
    }         
    else {
        /* global array:
           time dimension: ldims=1, gdims=0
           in C array, it can only be the first dim
           in Fortran array, it can only be the last dim
           (always the slowest changing dim)
        */
        if (ldims[0] == 1 && gdims[*ndim-1] == 0) {
            /* first dimension is the time (C array)
             * ldims[0] = 1 but gdims does not contain time info and 
             * gdims[0] is 1st data dimension and 
             * gdims is shorter by one value than ldims in case of C.
             * Therefore, gdims[*ndim-1] = 0 if there is a time dimension. 
             */
            *timedim = 0;
            // error check
            if (file_is_fortran && *ndim > 1) {
                fprintf(stderr,"ADIOS Error: this is a BP file with Fortran ordering but we found"
                        "an array to have time dimension in the first dimension. l:g:o = (");
                for (i=0; i < *ndim; i++) {
                    fprintf(stderr,"%llu:%llu:%llu%s", ldims[i], gdims[i], offsets[i], (i<*ndim-1 ? ", " : "") );
                }
                fprintf(stderr, ")\n");
            }
            (*dims)[0] = ntsteps;
            for (i=1; i < *ndim; i++) 
                 (*dims)[i]=gdims[i-1];
        }
        else if (ldims[*ndim-1] == 1 && gdims[*ndim-1] == 0) {
            // last dimension is the time (Fortran array)
            *timedim = *ndim - 1;
            if (!file_is_fortran && *ndim > 1) {
                fprintf(stderr,"ADIOS Error: this is a BP file with C array ordering but we found"
                        "an array to have time dimension in the last dimension. l:g:o = (");
                for (i=0; i < *ndim; i++) {
                    fprintf(stderr,"%llu:%llu:%llu%s", ldims[i], gdims[i], offsets[i], (i<*ndim-1 ? ", " : "") );
                }
                fprintf(stderr, ")\n");
            }
            for (i=0; i < *ndim-1; i++) 
                 (*dims)[i]=gdims[i];
             (*dims)[*timedim] = ntsteps;
        } else {
            // no time dimenstion
            for (i=0; i < *ndim; i++) 
                 (*dims)[i]=gdims[i];
        }
    }
}

ADIOS_VARINFO * common_read_inq_var (ADIOS_GROUP *gp, const char * varname) 
{
    int varid = common_read_find_var(gp, varname);
    if (varid < 0)
        return NULL;
    return common_read_inq_var_byid(gp, varid);
}

ADIOS_VARINFO * common_read_inq_var_byid (ADIOS_GROUP *gp, int varid)
{
    struct BP_GROUP      * gh;
    struct BP_FILE       * fh;
    ADIOS_VARINFO * vi;
    int file_is_fortran;
    struct adios_index_var_struct_v1 * var_root;
    int i,k;

    adios_errno = 0;
    if (!gp) {
        error(err_invalid_group_struct, "Null pointer passed as group to adios_inq_var()");
        return NULL;
    }
    gh = (struct BP_GROUP *) gp->gh;
    if (!gh) {
        error(err_invalid_group_struct, "Invalid ADIOS_GROUP struct: .gh group handle is NULL!");
        return NULL;
    }
    fh = gh->fh;
    if (!fh) {
        error(err_invalid_group_struct, "Invalid ADIOS_GROUP struct: .gh->fh file handle is NULL!");
        return NULL;
    }
    if (varid < 0 || varid >= gh->vars_count) {
        error(err_invalid_varid, "Invalid variable id %d (allowed 0..%d)", varid, gh->vars_count);
        return NULL;
    }
    vi = (ADIOS_VARINFO *) malloc(sizeof(ADIOS_VARINFO));
    if (!vi) {
        error( err_no_memory, "Could not allocate memory for variable info");
        return NULL;
    }


    file_is_fortran = (fh->pgs_root->adios_host_language_fortran == adios_flag_yes);
    
    var_root = gh->vars_root; /* first variable of this group. Need to traverse the list */
    for (i=0; i<varid && var_root; i++) {
        var_root = var_root->next;
    }

    if (i!=varid) {
        error (err_corrupted_variable, "Variable id=%d is valid but was not found in internal data structures!",varid);
        return NULL; 
    }


    vi->varid = varid;

    vi->type = var_root->type;
    if (!var_root->characteristics_count) {
        error(err_corrupted_variable, "Variable %s does not have information on dimensions", 
              gp->var_namelist[varid]);
        free(vi);
        return NULL;
    }

    /* Get value or min/max */
    common_read_get_characteristics (var_root, vi);

    common_read_get_dimensions (var_root, fh->tidx_stop - fh->tidx_start + 1, file_is_fortran, 
                          &(vi->ndim), &(vi->dims), &(vi->timedim));
            
    if (file_is_fortran != futils_is_called_from_fortran()) {
        /* If this is a Fortran written file and this is called from C code,  
           or this is a C written file and this is called from Fortran code ==>
           We need to reverse the order of the dimensions */
        swap_order(vi->ndim, vi->dims, &(vi->timedim));
        /*printf("File was written from %s and read now from %s, so we swap order of dimensions\n",
                (file_is_fortran ? "Fortran" : "C"), (futils_is_called_from_fortran() ? "Fortran" : "C"));*/
    }
    
    return vi;
}

void common_read_free_varinfo (ADIOS_VARINFO *vp)
{
    if (vp) {
        if (vp->dims)   free(vp->dims);
        if (vp->value)  free(vp->value);
        if (vp->gmin && vp->gmin != vp->value)   free(vp->gmin);
        if (vp->gmax && vp->gmax != vp->value)   free(vp->gmax);
        //if (vp->mins)   free(vp->mins);
        //if (vp->maxs)   free(vp->maxs);
        free(vp);
    }
}

#define MPI_FILE_READ_OPS                           \
        bp_realloc_aligned(fh->b, slice_size);      \
        fh->b->offset = 0;                          \
                                                    \
        MPI_File_seek (fh->mpi_fh                   \
                      ,(MPI_Offset)slice_offset     \
                      ,MPI_SEEK_SET                 \
                      );                            \
                                                    \
        MPI_File_read (fh->mpi_fh                   \
                      ,fh->b->buff                  \
                      ,slice_size                   \
                      ,MPI_BYTE                     \
                      ,&status                      \
                      );                            \
        fh->b->offset = 0;                          \

//We also need to be able to read old .bp which doesn't have 'payload_offset'
#define MPI_FILE_READ_OPS1                                                                  \
        MPI_File_seek (fh->mpi_fh                                                           \
                      ,(MPI_Offset) var_root->characteristics[start_idx + idx].offset       \
                      ,MPI_SEEK_SET);                                                       \
        MPI_File_read (fh->mpi_fh, fh->b->buff, 8, MPI_BYTE, &status);                      \
        tmpcount= *((uint64_t*)fh->b->buff);                                                \
                                                                                            \
        bp_realloc_aligned(fh->b, tmpcount + 8);                                            \
        fh->b->offset = 0;                                                                  \
                                                                                            \
        MPI_File_seek (fh->mpi_fh                                                           \
                      ,(MPI_Offset) (var_root->characteristics[start_idx + idx].offset)     \
                      ,MPI_SEEK_SET);                                                       \
        MPI_File_read (fh->mpi_fh, fh->b->buff, tmpcount + 8, MPI_BYTE, &status);           \
        fh->b->offset = 0;                                                                  \
        adios_parse_var_data_header_v1 (fh->b, &var_header);                                \


int64_t common_read_read_var (ADIOS_GROUP * gp, const char * varname,
                        const uint64_t * start, const uint64_t * count,
                        void * data)
{
    int varid = common_read_find_var(gp, varname);
    if (varid < 0)
        return -1;
    return common_read_read_var_byid(gp, varid, start, count, data);
}

int64_t common_read_read_var_byid (ADIOS_GROUP    * gp,
                             int              varid,
                             const uint64_t  * start,
                             const uint64_t  * count,
                             void           * data)
{
    struct BP_GROUP      * gh;
    struct BP_FILE       * fh;
    int file_is_fortran;
    struct adios_index_var_struct_v1 * var_root;
    struct adios_var_header_struct_v1 var_header;
    struct adios_var_payload_struct_v1 var_payload;
    int    i,j,k, idx, timestep;
    int    start_time, stop_time;
    int    pgoffset, pgcount, start_idx;
    int    ndim, ndim_notime;  
    uint64_t size;
    uint64_t *dims;
    uint64_t ldims[32];
    uint64_t gdims[32];
    uint64_t offsets[32];
    uint64_t datasize, nloop, dset_stride,var_stride, total_size=0, items_read;
    uint64_t count_notime[32], start_notime[32];
    MPI_Status status;
    int timedim = -1;
    int rank;
    int is_global = 0;
    int size_of_type;
    uint64_t slice_offset;
    uint64_t slice_size;
    uint64_t tmpcount = 0;
    uint64_t datatimeoffset = 0; // offset in data to write a given timestep


    adios_errno = 0;
    if (!gp) {
        error(err_invalid_group_struct, "Null pointer passed as group to adios_read_var()");
        return -adios_errno;
    }
    gh = (struct BP_GROUP *) gp->gh;
    if (!gh) {
        error(err_invalid_group_struct, "Invalid ADIOS_GROUP struct: .gh group handle is NULL!");
        return -adios_errno;
    }
    fh = gh->fh;
    if (!fh) {
        error(err_invalid_group_struct, "Invalid ADIOS_GROUP struct: .gh->fh file handle is NULL!");
        return -adios_errno;
    }
    if (varid < 0 || varid >= gh->vars_count) {
        error(err_invalid_varid, "Invalid variable id %d (allowed 0..%d)", varid, gh->vars_count);
        return -adios_errno;
    }
    

    file_is_fortran = (fh->pgs_root->adios_host_language_fortran == adios_flag_yes);
    
    var_root = gh->vars_root; /* first variable of this group. Need to traverse the list */
    for (i=0; i<varid && var_root; i++) {
        var_root = var_root->next;
    }

    if (i!=varid) {
        error (err_corrupted_variable, "Variable id=%d is valid but was not found in internal data structures!",varid);
        return -adios_errno; 
    }

    /* Get dimensions and flip if caller != writer language */
    common_read_get_dimensions (var_root, fh->tidx_stop - fh->tidx_start + 1, file_is_fortran, 
                          &ndim, &dims, &timedim);

    if ( file_is_fortran != futils_is_called_from_fortran() ) 
        swap_order(ndim, dims, &timedim);

    /* Get the timesteps we need to read */
    if (timedim > -1) {
        start_time = start[timedim] + fh->tidx_start;
        stop_time = start_time + count[timedim] - 1;
    } else {
        // timeless variable
        start_time = fh->tidx_start;
        stop_time = fh->tidx_start;
    }

    /* Take out the time dimension from start[] and count[] */
    if (timedim == -1) {
        for (i = 0; i < ndim; i++) {
             count_notime[i] = count[i];
             start_notime[i] = start[i];
        }
        ndim_notime = ndim;
    } else {
        j = 0;
        for (i = 0; i < timedim; i++) {
             count_notime[j] = count[i];
             start_notime[j] = start[i];
             j++;
        }
        i++; // skip timedim
        for (; i < ndim; i++) {
             count_notime[j] = count[i];
             start_notime[j] = start[i];
             j++;
        }
        ndim_notime = ndim-1;
    }
    
    /* items_read = how many data elements are we going to read in total (per timestep) */
    items_read = 1;
    for (i = 0; i < ndim_notime; i++)
        items_read *= count_notime[i];
    
    
    MPI_Comm_rank(gh->fh->comm, &rank);

    size_of_type = bp_get_type_size (var_root->type, var_root->characteristics [0].value);

    /* For each timestep, do reading separately (they are stored in different sets of process groups */
    for (timestep = start_time; timestep <= stop_time; timestep++) {

        // pgoffset = the starting offset for the given time step
        // pgcount  = number of process groups of that time step
        pgoffset = fh->gvar_h->time_index[0][gh->group_id][timestep - fh->tidx_start];
        pgcount = fh->gvar_h->time_index[1][gh->group_id][timestep - fh->tidx_start];
        start_idx = -1;
        for (i=0;i<var_root->characteristics_count;i++) {
            if (   (  var_root->characteristics[i].offset > fh->gvar_h->pg_offsets[pgoffset])
                && (  (i == var_root->characteristics_count-1) 
                    ||(  var_root->characteristics[i].offset < fh->gvar_h->pg_offsets[pgoffset + 1]))
               ) 
            {
                start_idx = i;
                break;
            }
        }

        if (start_idx<0) {
            error(err_no_data_at_timestep,"Variable (id=%d) has no data at %d time step",
                varid, timestep);
            return -adios_errno;
        }

        if (ndim_notime == 0) {
            /* READ A SCALAR VARIABLE */

            slice_size = size_of_type;
            idx = 0; // macros below need it

            if (var_root->type == adios_string) {
                // strings are stored without \0 in file
                // size_of_type here includes \0 so decrease by one
                size_of_type--;
            }

            if (var_root->characteristics[start_idx+idx].payload_offset > 0) {
                slice_offset = var_root->characteristics[start_idx+idx].payload_offset;
                MPI_FILE_READ_OPS
            } else {
                slice_offset = 0;
                MPI_FILE_READ_OPS1
            }

            memcpy((char *)data+total_size, fh->b->buff + fh->b->offset, size_of_type);
            if (fh->mfooter.change_endianness == adios_flag_yes) {
                change_endianness((char *)data+total_size, size_of_type, var_root->type);
            }

            if (var_root->type == adios_string) {
                // add \0 to the end of string
                // size_of_type here is the length of string
                // FIXME: how would this work for strings written over time?
                ((char*)data+total_size)[size_of_type] = '\0';
            }

            total_size += size_of_type;
            continue;
        }

         /* READ AN ARRAY VARIABLE */
        int * idx_table = (int *) malloc (sizeof(int) * pgcount);

        uint64_t write_offset = 0;
        int npg = 0;
        tmpcount = 0;
        if (pgcount > var_root->characteristics_count)
            pgcount = var_root->characteristics_count;

        // loop over the list of pgs to read from one-by-one
        for (idx = 0; idx < pgcount; idx++) {
            int flag;
            datasize = 1;
            nloop = 1;
            var_stride = 1;
            dset_stride = 1;
            idx_table[idx] = 1;
            uint64_t payload_size = size_of_type;
    
            /* Each pg can have a different sized array, so we need the actual dimensions from it */
            is_global = common_read_get_dimensioncharacteristics( &(var_root->characteristics[start_idx + idx]),
                                                            ldims, gdims, offsets);
            if (!is_global) {
                // we use gdims below, which is 0 for a local array; set to ldims here
                for (j = 0; j< ndim; j++) {
                    gdims[j]=ldims[j];
                }
            }

            if (file_is_fortran != futils_is_called_from_fortran()) {
                i=-1;
                swap_order(ndim, gdims,   &(i)); // i is dummy 
                swap_order(ndim, ldims,   &(i));
                swap_order(ndim, offsets, &(i));
            }
            
            /* take out the time dimension */
            if (timedim > -1) {
                for (i = timedim; i < ndim-1; i++) {
                    ldims[i] = ldims[i+1];
                    gdims[i] = gdims[i+1];
                    offsets[i] = offsets[i+1];
                }
            }
                
            for (j = 0; j < ndim_notime; j++) {
    
                payload_size *= ldims [j];
    
                if ( (count_notime[j] > gdims[j]) 
                  || (start_notime[j] > gdims[j]) 
                  || (start_notime[j] + count_notime[j] > gdims[j])){
                    error( err_out_of_bound, "Error: Variable (id=%d) out of bound ("
                        "the data in dimension %d to read is %llu elements from index %llu"
                        " but the actual data is [0,%llu])",
                        varid, j+1, count_notime[j], start_notime[j], gdims[j] - 1);
                    return -adios_errno;
                }
    
                /* check if there is any data in this pg and this dimension to read in */
                flag = (offsets[j] >= start_notime[j] 
                        && offsets[j] < start_notime[j] + count_notime[j])
                    || (offsets[j] < start_notime[j]
                        && offsets[j] + ldims[j] > start_notime[j] + count_notime[j]) 
                    || (offsets[j] + ldims[j] > start_notime[j] 
                        && offsets[j] + ldims[j] <= start_notime[j] + count_notime[j]);
                idx_table [idx] = idx_table[idx] && flag;
            }
            
            if ( !idx_table[idx] ) {
                continue;
            }
            ++npg;
    
            /* determined how many (fastest changing) dimensions can we read in in one read */
            int hole_break; 
            for (i = ndim_notime - 1; i > -1; i--) {
                if (offsets[i] == start_notime[i] && ldims[i] == count_notime[i]) {
                    datasize *= ldims[i];
                }
                else
                    break;
            }
    
            hole_break = i;
            slice_offset = 0;
            slice_size = 0;
    
            if (hole_break == -1) {
                /* The complete read happens to be exactly one pg, and the entire pg */
                /* This means we enter this only once, and npg=1 at the end */
                /* This is a rare case. FIXME: cannot eliminate this? */
                slice_size = payload_size;
    
                if (var_root->characteristics[start_idx + idx].payload_offset > 0) {
                    slice_offset = var_root->characteristics[start_idx + idx].payload_offset;
                    MPI_FILE_READ_OPS
                } else {
                    slice_offset = 0;
                    MPI_FILE_READ_OPS1
                }
    
                memcpy( (char *)data, fh->b->buff + fh->b->offset, slice_size);
                if (fh->mfooter.change_endianness == adios_flag_yes) {
                    change_endianness(data, slice_size, var_root->type);
                }
            }
            else if (hole_break == 0) 
            {
                /* The slowest changing dimensions should not be read completely but
                   we still need to read only one block */
                int isize;
                uint64_t size_in_dset = 0;
                uint64_t offset_in_dset = 0;
    
                isize = offsets[0] + ldims[0];
                if (start_notime[0] >= offsets[0]) {
                    // head is in
                    if (start_notime[0]<isize) {
                        if (start_notime[0] + count_notime[0] > isize)
                            size_in_dset = isize - start_notime[0];
                        else
                            size_in_dset = count_notime[0];
                        offset_in_dset = start_notime[0] - offsets[0];
                    }
                }
                else {
                    // middle is in
                    if (isize < start_notime[0] + count_notime[0])
                        size_in_dset = ldims[0];
                    else
                    // tail is in
                        size_in_dset = count_notime[0] + start_notime[0] - offsets[0];
                    offset_in_dset = 0;
                }
    
                slice_size = size_in_dset * datasize * size_of_type;
    
                if (var_root->characteristics[start_idx + idx].payload_offset > 0) {
                    slice_offset = var_root->characteristics[start_idx + idx].payload_offset 
                                 + offset_in_dset * datasize * size_of_type;
                    MPI_FILE_READ_OPS
                } else {
                    slice_offset = 0;
                    MPI_FILE_READ_OPS1
                }
    
                memcpy ((char *)data + write_offset, fh->b->buff + fh->b->offset, slice_size);
                if (fh->mfooter.change_endianness == adios_flag_yes) {
                    change_endianness((char *)data + write_offset, slice_size, var_root->type);
                }
    
                write_offset +=  slice_size;
            }
            else 
            {

                uint64_t stride_offset = 0;
                int isize;
                uint64_t size_in_dset[10];
                uint64_t offset_in_dset[10];
                uint64_t offset_in_var[10];
                memset(size_in_dset, 0 , 10 * 8);
                memset(offset_in_dset, 0 , 10 * 8);
                memset(offset_in_var, 0 , 10 * 8);
                int hit = 0;
                for ( i = 0; i < ndim_notime ; i++) {
                    isize = offsets[i] + ldims[i];
                    if (start_notime[i] >= offsets[i]) {
                        // head is in
                        if (start_notime[i]<isize) {
                            if (start_notime[i] + count_notime[i] > isize)
                                size_in_dset[i] = isize - start_notime[i];
                            else
                                size_in_dset[i] = count_notime[i];
                            offset_in_dset[i] = start_notime[i] - offsets[i];
                            offset_in_var[i] = 0;
                            hit = 1 + hit * 10;
                        }
                        else
                            hit = -1;
                    }
                    else {
                        // middle is in
                        if (isize < start_notime[i] + count_notime[i]) {
                            size_in_dset[i] = ldims[i];
                            hit = 2 + hit * 10;
                        }
                        else {
                            // tail is in
                            size_in_dset[i] = count_notime[i] + start_notime[i] - offsets[i];
                            hit = 3 + hit * 10;
                        }
                        offset_in_dset[i] = 0;
                        offset_in_var[i] = offsets[i] - start_notime[i];
                    }
                }
    
                datasize = 1;
                var_stride = 1;
    
                for ( i = ndim_notime-1; i >= hole_break; i--) {
                    datasize *= size_in_dset[i];
                    dset_stride *= ldims[i];
                    var_stride *= count_notime[i];
                }
    
                uint64_t start_in_payload = 0, end_in_payload = 0, s = 1;
                for (i = ndim_notime - 1; i > -1; i--) {
                    start_in_payload += s * offset_in_dset[i] * size_of_type;
                    end_in_payload += s * (offset_in_dset[i] + size_in_dset[i] - 1) * size_of_type;
                    s *= ldims[i];
                }
    
                slice_size = end_in_payload - start_in_payload + 1 * size_of_type;
    
                if (var_root->characteristics[start_idx + idx].payload_offset > 0) {
                    slice_offset =  var_root->characteristics[start_idx + idx].payload_offset
                                  + start_in_payload;
                    MPI_FILE_READ_OPS
    
                    for ( i = 0; i < ndim_notime ; i++) {
                        offset_in_dset[i] = 0;
                    }
                } else {
                    slice_offset =  start_in_payload;
                    MPI_FILE_READ_OPS1
                }
    
                uint64_t var_offset = 0;
                uint64_t dset_offset = 0;
                for ( i = 0; i < hole_break; i++) {
                    nloop *= size_in_dset[i];
                }
    
                for ( i = 0; i < ndim_notime ; i++) {
                    var_offset = offset_in_var[i] + var_offset * count_notime[i];
                    dset_offset = offset_in_dset[i] + dset_offset * ldims[i];
                }
    
                copy_data (data
                          ,fh->b->buff + fh->b->offset
                          ,0
                          ,hole_break
                          ,size_in_dset
                          ,ldims
                          ,count_notime
                          ,var_stride
                          ,dset_stride
                          ,var_offset
                          ,dset_offset
                          ,datasize
                          ,size_of_type 
                          );
            }
        }  // end for (idx ... loop over pgs
    
        free (idx_table);
    
        total_size += items_read * size_of_type;
        // shift target pointer for next read in
        data = (char *)data + (items_read * size_of_type);

    } // end for (timestep ... loop over timesteps

    free (dims);

    return total_size;
}

const char * common_read_type_to_string (enum ADIOS_DATATYPES type)
{
    switch (type)
    {
        case adios_unsigned_byte:    return "unsigned byte";
        case adios_unsigned_short:   return "unsigned short";
        case adios_unsigned_integer: return "unsigned integer";
        case adios_unsigned_long:    return "unsigned long long";

        case adios_byte:             return "byte";
        case adios_short:            return "short";
        case adios_integer:          return "integer";
        case adios_long:             return "long long";

        case adios_real:             return "real";
        case adios_double:           return "double";
        case adios_long_double:      return "long double";

        case adios_string:           return "string";
        case adios_complex:          return "complex";
        case adios_double_complex:   return "double complex";

        default:
        {
            static char buf [50];
            sprintf (buf, "(unknown: %d)", type);
            return buf;
        }
    }
}

int common_read_type_size(enum ADIOS_DATATYPES type, void *data)
{
    return bp_get_type_size(type, data);
}


void common_read_print_groupinfo (ADIOS_GROUP *gp) 
{
    int i;
    printf ("---------------------------\n");
    printf ("     var information\n");
    printf ("---------------------------\n");
    printf ("    var id\tname\n");
    if (gp->var_namelist) {
        for (i=0; i<gp->vars_count; i++)
            printf("\t%d)\t%s\n", i, gp->var_namelist[i]);
    }
    printf ("---------------------------\n");
    printf ("     attribute information\n");
    printf ("---------------------------\n");
    printf ("    attr id\tname\n");
    if (gp->attr_namelist) {
        for (i=0; i<gp->attrs_count; i++)
            printf("\t%d)\t%s\n", i, gp->attr_namelist[i]);
    }
    return;
}


void common_read_print_fileinfo (ADIOS_FILE *fp) 
{
    int i;
    printf ("---------------------------\n");
    printf ("     group information\n");
    printf ("---------------------------\n");
    printf ("\t# of groups:\t%d\n"
        "\t# of variables:\t%d\n"
        "\t# of attributes:%d\n"
        "\t# of timesteps:\t%d starting from %d\n",
        fp->groups_count,
        fp->vars_count,
        fp->attrs_count,
        fp->ntimesteps,
        fp->tidx_start);
    printf ("\t----------------\n");
    printf ("\tgroup id\tname\n");
    if (fp->group_namelist) {
        for (i=0; i<fp->groups_count; i++)
            printf("\t  %d)\t%s\n", i, fp->group_namelist[i]);
    }
    return;
}

#ifdef __cplusplus
extern "C"  /* prevent C++ name mangling */
#endif
