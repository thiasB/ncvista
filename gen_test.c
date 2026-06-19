/* Generate a small CF-style test file: temperature(time,lat,lon) in degC with a
 * udunits time axis, and an anomaly field, plus a land/sea mask with fill. */
#include <netcdf.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NT 24
#define NLAT 73
#define NLON 144

static void chk(int s){ if(s){ fprintf(stderr,"nc err: %s\n",nc_strerror(s)); exit(1);} }

int main(void){
    int nc, dt,dla,dlo,dbnds, vt,vla,vlo, vtemp,vanom,vmask,vtbnds;
    chk(nc_create("sample.nc", NC_CLOBBER|NC_NETCDF4, &nc));
    chk(nc_def_dim(nc,"time",NT,&dt));
    chk(nc_def_dim(nc,"lat",NLAT,&dla));
    chk(nc_def_dim(nc,"lon",NLON,&dlo));
    chk(nc_def_dim(nc,"bnds",2,&dbnds));

    chk(nc_def_var(nc,"time",NC_DOUBLE,1,&dt,&vt));
    chk(nc_put_att_text(nc,vt,"units",strlen("days since 2000-01-01 00:00:00"),"days since 2000-01-01 00:00:00"));
    chk(nc_put_att_text(nc,vt,"calendar",8,"standard"));
    chk(nc_put_att_text(nc,vt,"standard_name",4,"time"));
    chk(nc_put_att_text(nc,vt,"bounds",9,"time_bnds"));

    /* Supporting variable defined early in file order; should sort to the end. */
    {int tbd[2]={dt,dbnds}; chk(nc_def_var(nc,"time_bnds",NC_DOUBLE,2,tbd,&vtbnds));}

    chk(nc_def_var(nc,"lat",NC_DOUBLE,1,&dla,&vla));
    chk(nc_put_att_text(nc,vla,"units",13,"degrees_north"));
    chk(nc_def_var(nc,"lon",NC_DOUBLE,1,&dlo,&vlo));
    chk(nc_put_att_text(nc,vlo,"units",12,"degrees_east"));

    int dims3[3]={dt,dla,dlo};
    chk(nc_def_var(nc,"temperature",NC_FLOAT,3,dims3,&vtemp));
    chk(nc_put_att_text(nc,vtemp,"units",15,"degree_Celsius"));
    chk(nc_put_att_text(nc,vtemp,"long_name",19,"surface temperature"));
    float fill=9.96921e36f;
    chk(nc_put_att_float(nc,vtemp,"_FillValue",NC_FLOAT,1,&fill));

    chk(nc_def_var(nc,"anomaly",NC_FLOAT,3,dims3,&vanom));
    chk(nc_put_att_text(nc,vanom,"units",1,"K"));
    chk(nc_put_att_text(nc,vanom,"long_name",20,"temperature anomaly "));

    chk(nc_def_var(nc,"mask",NC_FLOAT,2,&dims3[1],&vmask));
    chk(nc_put_att_text(nc,vmask,"long_name",13,"land-sea mask"));

    /* Global attributes (shown after the variables in the metadata window). */
    chk(nc_put_att_text(nc,NC_GLOBAL,"title",25,"ncvista synthetic example"));
    chk(nc_put_att_text(nc,NC_GLOBAL,"Conventions",6,"CF-1.8"));
    chk(nc_put_att_text(nc,NC_GLOBAL,"institution",30,"Potsdam Institute (synthetic)"));

    chk(nc_enddef(nc));

    double *time=malloc(NT*sizeof(double));
    for(int t=0;t<NT;t++) time[t]=t*15.0;          /* every 15 days */
    double *lat=malloc(NLAT*sizeof(double));
    for(int j=0;j<NLAT;j++) lat[j]=-90.0+j*(180.0/(NLAT-1));
    double *lon=malloc(NLON*sizeof(double));
    for(int i=0;i<NLON;i++) lon[i]=i*(360.0/NLON);
    chk(nc_put_var_double(nc,vt,time));
    chk(nc_put_var_double(nc,vla,lat));
    chk(nc_put_var_double(nc,vlo,lon));

    double *tbnds=malloc((size_t)NT*2*sizeof(double));
    for(int t=0;t<NT;t++){ tbnds[2*t]=time[t]-7.5; tbnds[2*t+1]=time[t]+7.5; }
    chk(nc_put_var_double(nc,vtbnds,tbnds));

    float *temp=malloc((size_t)NT*NLAT*NLON*sizeof(float));
    float *anom=malloc((size_t)NT*NLAT*NLON*sizeof(float));
    for(int t=0;t<NT;t++){
        double phase=2*M_PI*t/NT;
        for(int j=0;j<NLAT;j++){
            double la=lat[j]*M_PI/180.0;
            for(int i=0;i<NLON;i++){
                double lo=lon[i]*M_PI/180.0;
                size_t k=((size_t)t*NLAT+j)*NLON+i;
                temp[k]=(float)(30.0*cos(la) - 15.0 + 8.0*sin(phase)*cos(la)
                                + 5.0*sin(2*lo));
                anom[k]=(float)(4.0*sin(3*lo+phase)*cos(2*la));
                if(j<2 && i<2) temp[k]=fill;          /* a few fills */
            }
        }
    }
    chk(nc_put_var_float(nc,vtemp,temp));
    chk(nc_put_var_float(nc,vanom,anom));

    float *mask=malloc((size_t)NLAT*NLON*sizeof(float));
    for(int j=0;j<NLAT;j++) for(int i=0;i<NLON;i++)
        mask[(size_t)j*NLON+i]=(float)((sin(lat[j]*0.1)+cos(lon[i]*0.1))>0?1:0);
    chk(nc_put_var_float(nc,vmask,mask));

    chk(nc_close(nc));
    printf("wrote sample.nc\n");
    return 0;
}
