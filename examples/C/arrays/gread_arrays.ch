adios_groupsize = 0;
adios_totalsize = 0;
adios_group_size (adios_handle, adios_groupsize, &adios_totalsize);
adios_buf_size = 4;
adios_read (adios_handle, "NX", &NX, adios_buf_size);
adios_buf_size = 4;
adios_read (adios_handle, "NY", &NY, adios_buf_size);
adios_buf_size = 8 * (NX) * (NY);
adios_read (adios_handle, "var_double_2Darray", var_double_2Darray, adios_buf_size);
adios_buf_size = 4 * (NX);
adios_read (adios_handle, "var_int_1Darray", var_int_1Darray, adios_buf_size);
