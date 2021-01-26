/*
 *  Copyright (C) 2019 Tyson B. Littenberg (MSFC-ST12), Kristen Lackeos, Neil J. Cornish
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with with program; see the file COPYING. If not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 *  MA  02111-1307  USA
 */



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <getopt.h>


#include <gsl/gsl_sort.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_sort_vector.h>

#include <LISA.h>
#include <GMM_with_EM.h>


#include "GalacticBinary.h"
#include "GalacticBinaryIO.h"
#include "GalacticBinaryMath.h"
#include "GalacticBinaryModel.h"
#include "GalacticBinaryPrior.h"
#include "GalacticBinaryWaveform.h"
#include "GalacticBinaryCatalog.h"

/* ============================  MAIN PROGRAM  ============================ */
static void print_usage_catalog();
static void parse_catalog(int argc, char **argv, struct Data *data, struct Orbit *orbit, struct Flags *flags, int Nmax, double *Tcatalog, size_t *NMODE, size_t *NTHIN);
static int gaussian_mixture_model_wrapper(double **ranges, struct Flags *flags, struct Entry *entry, char *outdir, size_t NP, size_t NMODE, size_t NTHIN, gsl_rng *seed, double *BIC);

int main(int argc, char *argv[])
{
    
    /* ************************************************************** */
    /*             Allocate & Initialize Data Structures              */
    /* ************************************************************** */
    
    int NTEMP = 1;     //needed size of data structure
    size_t NMODE = 16; //default size of GMM
    size_t NTHIN = 1;  //thinning rate of chain
    
    /* Allocate data structures */
    struct Flags *flags = malloc(sizeof(struct Flags));
    struct Orbit *orbit = malloc(sizeof(struct Orbit));
    struct Data  *data  = malloc(sizeof(struct Data));
    struct Data *data_old = malloc(sizeof(struct Data));
    
    double Tcatalog; //duration of previous analysis (for source heritage)
    
    /* Parse command line and set defaults/flags */
    data->t0 = malloc( NTEMP * sizeof(double) );
    
    parse_catalog(argc,argv,data,orbit,flags,NTEMP,&Tcatalog,&NMODE,&NTHIN);
    alloc_data(data, flags);
    data->qmin = (int)(data->fmin*data->T);
    data->qmax = data->qmin + data->N;
    
    data_old->T    = Tcatalog;
    data_old->qmin = (int)(data->fmin*data_old->T);
    data_old->qmax = data_old->qmin + data->N;
    
    //File containing chain samples
    FILE *chain_file = fopen(data->fileName,"r");
    
    //Orbits
    /* Load spacecraft ephemerides */
    switch(flags->orbit)
    {
        case 0:
            initialize_analytic_orbit(orbit);
            break;
        case 1:
            initialize_numeric_orbit(orbit);
            break;
        default:
            fprintf(stderr,"unsupported orbit type\n");
            free_orbit(orbit);
            return(1);
            break;
    }
    
    // Get priors
    /* Load priors as used in MCMC */
    struct Model *model = malloc(sizeof(struct Model));
    alloc_model(model,data->DMAX,data->N,data->Nchannel, data->NP, flags->NT);
    set_uniform_prior(flags, model, data, 1);
    
    /* Reformat for catalog */
    //frequency & derivatives
    model->prior[0][0] /= data->T;
    model->prior[0][1] /= data->T;
    if(data->NP>7)
    {
        model->prior[7][0] /= data->T*data->T;
        model->prior[7][1] /= data->T*data->T;
    }
    if(data->NP>8)
    {
        model->prior[8][0] /= data->T*data->T*data->T;
        model->prior[8][1] /= data->T*data->T*data->T;
    }
    
    //alias for catalog->entry pointers used later on
    struct Entry *entry = NULL;
    
    struct Source *sample = NULL;
    sample = malloc(sizeof *sample);
    alloc_source(sample, data->N, data->Nchannel, data->NP);
    
    
    //count lines in chain file
    int N=0;
    while(!feof(chain_file))
    {
        scan_source_params(data, sample, chain_file);
        N++;
    }
    N--;
    rewind(chain_file);
    
    
    //selection criteria for catalog entries
    int matchFlag;          //track if sample matches any entries
    double Match;     //match between pairs of waveforms
    double Distance;  //distance between pairs of waveforms
    double tolerance = data->pmax; //tolerance on match to be considered associated
    double dqmax = 10;      //maximum frequency separation to try match calculation (in frequency bins)
    int downsample = 10;
    
    
    //Book-keeping
    int DMAX = data->DMAX; //maximum number of sources per chain sample (needs to be read in from above)
    int IMAX = N/DMAX;     //maximum number of chain samples (needs to be read in from above)
    int NMAX = N; //(absurd) upper limit on total number of catalog entries
    
    struct Catalog *catalog = NULL;
    catalog = malloc(sizeof(struct Catalog));
    catalog->N = 0; //start with 0 sources in catalog
    catalog->entry = malloc(NMAX*sizeof(struct Entry*));
    
    /* ************************************************************** */
    /*             Allocate & Initialize Instrument Model             */
    /* ************************************************************** */
    
    struct Noise *noise = NULL;
    noise = malloc(flags->NT*sizeof(struct Noise));
    alloc_noise(noise, data->N);
    
    //Noise model
    //Get noise spectrum for data segment
    if(flags->simNoise)
    {
        for(int n=0; n<data->N; n++)
        {
            double f = data->fmin + (double)(n)/data->T;
            if(strcmp(data->format,"phase")==0)
            {
                noise->SnA[n] = AEnoise(orbit->L, orbit->fstar, f);
                noise->SnE[n] = AEnoise(orbit->L, orbit->fstar, f);
            }
            else if(strcmp(data->format,"frequency")==0)
            {
                noise->SnA[n] = AEnoise_FF(orbit->L, orbit->fstar, f);
                noise->SnE[n] = AEnoise_FF(orbit->L, orbit->fstar, f);
            }
            else
            {
                fprintf(stderr,"Unsupported data format %s",data->format);
                exit(1);
            }
        }
    }
    else
    {
        double junk;
        FILE *noiseFile = fopen(flags->noiseFile,"r");
        for(int n=0; n<data->N; n++)
        {
            int check = 0;
            check += fscanf(noiseFile,"%lg",&junk); //);f
            check += fscanf(noiseFile,"%lg",&noise->SnA[n]);//A_med);
            check += fscanf(noiseFile,"%lg",&junk); //A_lo_50
            check += fscanf(noiseFile,"%lg",&junk);//A_hi_50);
            check += fscanf(noiseFile,"%lg",&junk);//A_lo_90);
            check += fscanf(noiseFile,"%lg",&junk);//A_hi_90);
            check += fscanf(noiseFile,"%lg",&noise->SnE[n]);//E_med);
            check += fscanf(noiseFile,"%lg",&junk);//E_lo_50);
            check += fscanf(noiseFile,"%lg",&junk);//E_hi_50);
            check += fscanf(noiseFile,"%lg",&junk);//E_lo_90);
            check += fscanf(noiseFile,"%lg",&junk);//E_hi_90);
            if(!check)
            {
                fprintf(stderr,"Error reading %s\n",flags->noiseFile);
                exit(1);
            }
        }
        fclose(noiseFile);
    }
    
    /* **************************************************************** */
    /*        First sample of the chain initializes entry list          */
    /* **************************************************************** */
    for(int d=0; d<DMAX; d++)
    {
        
        //parse source in first sample of chain file
        scan_source_params(data, sample, chain_file);
        
        //Book-keeping of waveform in time-frequency volume
        galactic_binary_alignment(orbit, data, sample);
        
        //calculate waveform model of sample
        galactic_binary(orbit, data->format, data->T, data->t0[0], sample->params, data->NP, sample->tdi->X, sample->tdi->A, sample->tdi->E, sample->BW, data->Nchannel);
        
        //check frequencies
        double q_sample = sample->f0 * data->T;
        if(q_sample > data->qmin+data->qpad && q_sample < data->qmax-data->qpad)
        {
            //add new source to catalog
            create_new_source(catalog, sample, noise, IMAX, data->N, sample->tdi->Nchannel, data->NP);
        }
    }
    
    
    /* ****************************************************************/
    /*            Now loop over the rest of the chain file            */
    /* ****************************************************************/
    
    //prevent multiple templates being added to the same source (only relevent for low SNR, low match threshold)
    int *entryFlag = malloc(NMAX*sizeof(int));
    
    fprintf(stdout,"\nLooping over chain file\n");
    for(int i=1; i<IMAX; i++)
    {
        if(i%(IMAX/100)==0)printProgress((double)i/(double)IMAX);
        
        for(int n=0; n<N; n++) entryFlag[n] = 0;
        
        //check each source in chain sample
        for(int d=0; d<DMAX; d++)
        {
            //parse source parameters
            scan_source_params(data, sample, chain_file);
            
            //find where the source fits in the measurement band
            galactic_binary_alignment(orbit, data, sample);
            
            //calculate waveform model of sample
            galactic_binary(orbit, data->format, data->T, data->t0[0], sample->params, data->NP, sample->tdi->X, sample->tdi->A, sample->tdi->E, sample->BW, data->Nchannel);
            
            double q_sample = sample->f0 * data->T;
            
            if(q_sample < data->qmin+data->qpad || q_sample > data->qmax-data->qpad) continue;
            
            if(i%downsample!=0) continue;
            
            //calculate match of sample and all entries
            matchFlag = 0;
            for(int n=0; n<catalog->N; n++)
            {
                entry = catalog->entry[n];
                
                //check frequency separation
                double q_entry  = entry->source[0]->f0 * data->T;
                
                if( fabs(q_entry-q_sample) > dqmax ) Match = -1.0;
                
                //calculate match
                else
                {
                    Match = waveform_match(sample, entry->source[0], noise);
                }
                
                if(Match > tolerance && !entryFlag[n])
                {
                    matchFlag = 1;
                    entryFlag[n] = 1;
                    Distance = waveform_distance(sample, entry->source[0], noise);
                    //append sample to entry
                    entry->match[entry->I] = Match;
                    entry->distance[entry->I] = Distance;
                    append_sample_to_entry(entry, sample, IMAX, data->N, data->Nchannel, data->NP);
                    
                    //stop looping over entries in catalog
                    break;
                }
                
            }//end loop over catalog entries
            
            
            //if the match tolerence is never met, add as new source
            if(!matchFlag)
            {
                entryFlag[catalog->N]=1;
                create_new_source(catalog, sample, noise, IMAX, data->N, data->Nchannel, data->NP);
            }
            
        }//end loop over sources in chain sample
        
    }//end loop over chain
    fprintf(stdout,"\n");
    fclose(chain_file);
    free(entryFlag);
    
    
    /* ****************************************************************/
    /*             Select entries that have enough weight             */
    /* ****************************************************************/
    double weight;
    double weight_threshold = 0.5; //what fraction of samples must an entry have to count?
    
    int detections = 0;            //number of entries that meet the weight threshold
    int *detection_index = malloc(NMAX*sizeof(int));//detection_index[NMAX];     //list of entry indicies for detected sources
    
    
    for(int n=0; n<catalog->N; n++)
    {
        
        weight = (double)catalog->entry[n]->I/(double)(IMAX/downsample);
        if(weight > weight_threshold)
        {
            //record index of catalog entry for detected source
            detection_index[detections] = n;
            
            //increment the number of detections
            detections++;
        }
    }
    fprintf(stdout,"\nNumber of discrete sources is %d.\n",detections);
    
    /* *************************************************************** */
    /*           Format selected entries as L3 data products           */
    /* *************************************************************** */
    
    char outdir[MAXSTRINGSIZE];
    sprintf(outdir,"catalog_%i",DMAX);
    mkdir(outdir,S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    
    char filename[128];
    sprintf(filename,"%s/entries.dat",outdir);
    FILE *catalogFile = fopen(filename,"w");
    
    double f_med;
    double *f_vec;
    size_t *index;
    int i_med;
    
    fprintf(stdout,"\nPost processing events\n");
    for(int d=0; d<detections; d++)
    {
        printProgress((double)(d+1)/(double)detections);
        
        int n = detection_index[d];
        entry = catalog->entry[n];
        
        //get sample containing median frequency as identifier of source
        f_vec = calloc(entry->I,sizeof(double));
        for(int i=0; i<entry->I; i++) f_vec[i] = entry->source[i]->f0;
        
        index = calloc(entry->I,(sizeof(size_t)));
        gsl_sort_index(index,f_vec,1,entry->I);
        i_med = index[entry->I/2];
        free(f_vec);
        free(index);
        
        f_med = entry->source[i_med]->f0;//gsl_stats_median_from_sorted_data(f_vec, 1, entry->I);
        
        entry->i = i_med;
        
        //replace stored SNR with median sample
        entry->SNR = snr(entry->source[i_med],noise);
        
        //name source based on median frequency
        sprintf(entry->name,"LDC%010li",(long)(f_med*1e10));
        
        //print point estimate parameters at median frequency
        sprintf(filename, "%s/%s_params.dat", outdir, entry->name);
        FILE *out = fopen(filename, "w");
        print_source_params(data,entry->source[i_med],out);
        fprintf(out,"\n");
        fclose(out);
        
        sprintf(filename, "%s/%s_waveform.dat", outdir,entry->name);
        out = fopen( filename, "w");
        
        
        
        
        
        struct Source *b=entry->source[0];
        int N = b->tdi->N;
        int NFFT = 2*N;
        double *b_A = malloc(NFFT*sizeof(double));
        double *b_E = malloc(NFFT*sizeof(double));
        for(int i=0; i<NFFT; i++)
        {
            b_A[i] = 0.0;
            b_E[i] = 0.0;
        }
        int qmin = b->qmin - b->imin;
        
        for(int i=0; i<b->BW; i++)
        {
            int j = i+b->qmin-qmin;
            if(j>-1 && j<N)
            {
                int i_re = 2*i;
                int i_im = i_re+1;
                int j_re = 2*j;
                int j_im = j_re+1;
                
                b_A[j_re] = b->tdi->A[i_re];
                b_A[j_im] = b->tdi->A[i_im];
                b_E[j_re] = b->tdi->E[i_re];
                b_E[j_im] = b->tdi->E[i_im];
                double f = (double)(j+data->qmin)/data->T;
                fprintf(out,"%.12g %lg %lg %lg %lg",f,b_A[j_re],b_A[j_im],b_E[j_re],b_E[j_im]);
                fprintf(out,"\n");
            }//check that index is in range
        }//loop over waveform bins
        
        free(b_A);
        free(b_E);
        fclose(out);
        
        //evidence for source related to number of visits in the chain
        entry->evidence = (double)(entry->I-1)/(double)(IMAX/downsample);
        
        fprintf(catalogFile,"%s %lg %lg\n",entry->name, entry->SNR, entry->evidence);
    }
    fflush(catalogFile);
    fprintf(stdout,"\n");
    
    
    /* *************************************************************** */
    /*           Check sources against previous catalog                */
    /* *************************************************************** */
    if(flags->catalog)
    {
        //parse file
        FILE *old_catalog_file = fopen(flags->catalogFile,"r");
        
        //count number of sources
        
        struct Source *old_catalog_entry = NULL;
        old_catalog_entry = malloc(sizeof(struct Source));
        alloc_source(old_catalog_entry, data->N, data->Nchannel, data->NP);
        
        struct Source *new_catalog_entry = NULL;
        new_catalog_entry = malloc(sizeof(struct Source));
        alloc_source(new_catalog_entry, data->N, data->Nchannel, data->NP);
        
        
        int Nsource = 0;
        while(!feof(old_catalog_file))
        {
            scan_source_params(data, old_catalog_entry, old_catalog_file);
            Nsource++;
        }
        Nsource--;
        rewind(old_catalog_file);
        
        sprintf(filename,"%s/history.dat",outdir);
        FILE *historyFile = fopen(filename,"w");
        for(int i=0; i<Nsource; i++)
        {
            scan_source_params(data_old, old_catalog_entry, old_catalog_file);
            
            //find where the source fits in the measurement band
            galactic_binary_alignment(orbit, data_old, old_catalog_entry);
            
            //find central bin of catalog event for current data
            double q_old_catalog_entry = old_catalog_entry->f0 * data_old->T;
            
            //check that source lives in current data segment (allow sources in padded region)
            if(q_old_catalog_entry < data_old->qmin || q_old_catalog_entry > data_old->qmax) continue;
            
            //calculate waveform model of sample at Tcatalog
            galactic_binary(orbit, data->format, data_old->T, data->t0[0], old_catalog_entry->params, data->NP, old_catalog_entry->tdi->X, old_catalog_entry->tdi->A, old_catalog_entry->tdi->E, old_catalog_entry->BW, data->Nchannel);
            
            //check against new catalog
            for(int d=0; d<detections; d++)
            {
                int n = detection_index[d];
                entry = catalog->entry[n];
                
                double q_new_catalog_entry = entry->source[entry->i]->f0 * data_old->T;
                
                //check that the sources are close enough to bother looking
                if(fabs(q_new_catalog_entry - q_old_catalog_entry) > dqmax) continue;
                
                copy_source(entry->source[entry->i], new_catalog_entry);
                
                //re-align where the source fits in the (old) measurement band
                map_params_to_array(new_catalog_entry, new_catalog_entry->params, data_old->T);
                galactic_binary_alignment(orbit, data_old, new_catalog_entry);
                
                //calculate waveform of entry at Tcatalog
                galactic_binary(orbit, data->format, data_old->T, data->t0[0], new_catalog_entry->params, data->NP, new_catalog_entry->tdi->X, new_catalog_entry->tdi->A, new_catalog_entry->tdi->E, new_catalog_entry->BW, data->Nchannel);
                
                
                Match = waveform_match(old_catalog_entry,new_catalog_entry,noise);
                
                if(Match>0.5)
                {
                    sprintf(entry->parent,"LDC%010li",(long)(old_catalog_entry->f0*1e10));
                    fprintf(historyFile,"%s %s\n",entry->parent,entry->name);
                }
                
            }
        }
        free_source(old_catalog_entry);
        free_source(new_catalog_entry);
    }
    
    
    
    /* *************************************************************** */
    /*           Save source detection parameters to file              */
    /* *************************************************************** */
    
    for(int d=0; d<detections; d++)
    {
        FILE *out;
        
        //print detection posterior samples
        int n = detection_index[d];
        entry = catalog->entry[n];
        
        sprintf(filename, "%s/%s_chain.dat", outdir,entry->name);
        out = fopen( filename, "w");
        
        //add parameters to file
        for(int k=0; k<entry->I; k++)
        {
            print_source_params(data,entry->source[k],out);
            fprintf(out,"%lg %lg %lg\n",snr(entry->source[k],noise),entry->match[k],entry->distance[k]);
        }
        fclose(out);
        
        //create and print individual source waveform reconstructions
        double ***hrec = malloc(data->N * sizeof(double **));
        for(int j=0; j<data->N; j++)
        {
            hrec[j] = malloc(data->Nchannel * sizeof(double *));
            
            //allocate and zero
            for(int k=0; k<data->Nchannel; k++)
            hrec[j][k] = calloc(entry->I , sizeof(double));
        }
        
        //insert waveform power
        for(int i=0; i<entry->I; i++)
        {
            for(int j=0; j<entry->source[i]->BW; j++)
            {
                
                int k = j+entry->source[i]->imin;
                
                if(k>-1 && k < data->N)
                {
                    int j_re = 2*j;
                    int j_im = j_re+1;
                    
                    hrec[k][0][i] = entry->source[i]->tdi->A[j_re]*entry->source[i]->tdi->A[j_re]+entry->source[i]->tdi->A[j_im]*entry->source[i]->tdi->A[j_im];
                    hrec[k][1][i] = entry->source[i]->tdi->E[j_re]*entry->source[i]->tdi->E[j_re]+entry->source[i]->tdi->E[j_im]*entry->source[i]->tdi->E[j_im];
                }
            }
        }
        
        //sort reconstructed power in each frequency bin and get median, CIs
        for(int j=0; j<data->N; j++)
        {
            for(int k=0; k<data->Nchannel; k++)
            {
                gsl_sort(hrec[j][k],1,entry->I);
            }
        }
        double A_med,A_lo_50,A_hi_50,A_lo_90,A_hi_90;
        double E_med,E_lo_50,E_hi_50,E_lo_90,E_hi_90;
        
        sprintf(filename, "%s/%s_power_reconstruction.dat", outdir,entry->name);
        out = fopen( filename, "w");
        
        
        for(int j=0; j<data->N; j++)
        {
            double f = (double)(j+data->qmin)/data->T;
            
            A_med   = gsl_stats_median_from_sorted_data   (hrec[j][0], 1, entry->I);
            A_lo_50 = gsl_stats_quantile_from_sorted_data (hrec[j][0], 1, entry->I, 0.25);
            A_hi_50 = gsl_stats_quantile_from_sorted_data (hrec[j][0], 1, entry->I, 0.75);
            A_lo_90 = gsl_stats_quantile_from_sorted_data (hrec[j][0], 1, entry->I, 0.05);
            A_hi_90 = gsl_stats_quantile_from_sorted_data (hrec[j][0], 1, entry->I, 0.95);
            
            E_med   = gsl_stats_median_from_sorted_data   (hrec[j][1], 1, entry->I);
            E_lo_50 = gsl_stats_quantile_from_sorted_data (hrec[j][1], 1, entry->I, 0.25);
            E_hi_50 = gsl_stats_quantile_from_sorted_data (hrec[j][1], 1, entry->I, 0.75);
            E_lo_90 = gsl_stats_quantile_from_sorted_data (hrec[j][1], 1, entry->I, 0.05);
            E_hi_90 = gsl_stats_quantile_from_sorted_data (hrec[j][1], 1, entry->I, 0.95);
            
            fprintf(out,"%.12g ",f);
            fprintf(out,"%lg ",A_med);
            fprintf(out,"%lg ",A_lo_50);
            fprintf(out,"%lg ",A_hi_50);
            fprintf(out,"%lg ",A_lo_90);
            fprintf(out,"%lg ",A_hi_90);
            fprintf(out,"%lg ",E_med);
            fprintf(out,"%lg ",E_lo_50);
            fprintf(out,"%lg ",E_hi_50);
            fprintf(out,"%lg ",E_lo_90);
            fprintf(out,"%lg ",E_hi_90);
            fprintf(out,"\n");
        }
        
        fclose(out);
        
        
        /* Gaussian mixture model fit to posterior */
        fprintf(stdout,"\nGaussian Mixture Model fit:\n");
        const gsl_rng_type *T = gsl_rng_default;
        const gsl_rng_type *Tsave = gsl_rng_default;
        const gsl_rng_type *Ttemp = gsl_rng_default;
        gsl_rng *r = gsl_rng_alloc(T);
        gsl_rng *rsave = gsl_rng_alloc(Tsave);
        gsl_rng *rtemp = gsl_rng_alloc(Ttemp);
        gsl_rng_env_setup();
        gsl_rng_set (r, 190521);
        
        
        int counter;
        int CMAX = 10;
        double BIC;
        double minBIC = 0.0;
        size_t minMODE = 0;
        
        sprintf(filename, "%s/%s_gmm_bic.dat", outdir,entry->name);
        out = fopen( filename, "w");
        
        counter = 0;
        gsl_rng_memcpy(rtemp, r);
        while(gaussian_mixture_model_wrapper(model->prior, flags, entry, outdir, (size_t)data->NP, NMODE, NTHIN, r, &BIC))
        {
            counter++;
            if(counter>CMAX)
            {
                fprintf(stderr,"WARNING:\n");
                fprintf(stderr,"Gaussian Mixture Model failed to converge for source %s\n",entry->name);
                NMODE/=2;
                counter = 0;
            }
            printf("\rRetry %i/%i: ",counter,CMAX);
        }
        
        
        /* ********************** */
        
        for(int i=0; i<data->N; i++)
        {
            for(int j=0; j<data->Nchannel; j++)
            {
                free(hrec[i][j]);
            }
            free(hrec[i]);
        }
        free(hrec);
    }//end loop over catalog entries
    
    
    free_noise(noise);
    //free_orbit(orbit);TODO: free_orbit() segfaults
    free(detection_index);
    
    return 0;
}


/* ============================  SUBROUTINES  ============================ */

void alloc_entry(struct Entry *entry, int IMAX)
{
    entry->I = 0;
    entry->source = malloc(IMAX*sizeof(struct Source*));
    entry->match  = malloc(IMAX*sizeof(double));
    entry->distance  = malloc(IMAX*sizeof(double));
    
}

void create_new_source(struct Catalog *catalog, struct Source *sample, struct Noise *noise, int IMAX, int NFFT, int Nchannel, int NP)
{
    int N = catalog->N;
    
    //allocate memory for new entry in catalog
    catalog->entry[N] = malloc(sizeof(struct Entry));
    struct Entry *entry = catalog->entry[N];
    
    alloc_entry(entry,IMAX);
    entry->source[entry->I] = malloc(sizeof(struct Source));
    alloc_source(entry->source[entry->I], NFFT, Nchannel, NP);
    
    //add sample to the catalog as the new entry
    copy_source(sample, entry->source[entry->I]);
    
    //store SNR of reference sample to set match criteria
    entry->SNR = snr(sample,noise);
    
    entry->match[entry->I] = 1.0;
    entry->distance[entry->I] = 0.0;
    
    entry->I++; //increment number of samples for entry
    catalog->N++;//increment number of entries for catalog
}

void append_sample_to_entry(struct Entry *entry, struct Source *sample, int IMAX, int NFFT, int Nchannel, int NP)
{
    //malloc source structure for this sample's entry
    entry->source[entry->I] = malloc(sizeof(struct Source));
    alloc_source(entry->source[entry->I], NFFT, Nchannel, NP);
    
    //copy chain sample into catalog entry
    copy_source(sample, entry->source[entry->I]);
    
    //increment number of stored samples for this entry
    entry->I++;
}

static void print_usage_catalog()
{
    fprintf(stdout,"\n");
    fprintf(stdout,"============== GBCATALOG Usage: ============ \n");
    fprintf(stdout,"REQUIRED:\n");
    fprintf(stdout,"       --chain-file  : chain file to be sorted into catalog\n");
    fprintf(stdout,"       --sources     : maximum number of sources (10)      \n");
    fprintf(stdout,"       --fmin        : minimum frequency                   \n");
    fprintf(stdout,"\n");
    fprintf(stdout,"OPTIONAL:\n");
    fprintf(stdout,"  -h | --help        : print help message and exit         \n");
    fprintf(stdout,"       --orbit       : orbit ephemerides file (2.5 GM MLDC)\n");
    fprintf(stdout,"       --samples     : number of frequency bins (2048)     \n");
    fprintf(stdout,"       --padding     : number of bins padded on segment (0)\n");
    fprintf(stdout,"       --duration    : duration of time segment (62914560) \n");
    fprintf(stdout,"       --match       : match threshold for waveforms (0.8) \n");
    fprintf(stdout,"       --frac-freq   : fractional frequency data (phase)   \n");
    fprintf(stdout,"       --f-double-dot: include f double dot in model       \n");
    fprintf(stdout,"       --links       : number of links [4->X,6->AE] (6)    \n");
    fprintf(stdout,"       --noise-file  : reconstructed noise model           \n");
    fprintf(stdout,"                       e.g., data/power_noise_t0_f0.dat    \n");
    fprintf(stdout,"       --catalog     : list of known sources               \n");
    fprintf(stdout,"       --Tcatalog    : observing time of previous catalog  \n");
    fprintf(stdout,"       --Nmode       : max number of GMM modes (16)        \n");
    fprintf(stdout,"       --thin        : factor for thinning chains in GMM   \n");
    fprintf(stdout,"--\n");
    fprintf(stdout,"EXAMPLE:\n");
    fprintf(stdout,"./gb_catalog --fmin 0.004 --samples 256 --duration 31457280 --sources 5 --chain-file chains/dimension_chain.dat.5");
    fprintf(stdout,"\n");
    fprintf(stdout,"\n");
    exit(EXIT_FAILURE);
}

static void parse_catalog(int argc, char **argv, struct Data *data, struct Orbit *orbit, struct Flags *flags, int Nmax, double *Tcatalog, size_t *NMODE, size_t *NTHIN)
{
    print_LISA_ASCII_art(stdout);
    print_version(stdout);
    
    if(argc==1) print_usage_catalog();
    
    int DMAX_default = 10;
    
    //Set defaults
    flags->orbit       = 0;
    flags->match       = 0;
    flags->NT          = 1;
    flags->NDATA       = 1;
    flags->DMAX        = DMAX_default;
    flags->catalog     = 0;
    flags->simNoise = 1; //hijack simNoise flag for noise model
    data->pmax      = 0.5; //default match tolerance for inclusion (hijacked pmax in data structure)
    *Tcatalog          = -1.0;
    
    /*
     default data format is 'phase'
     optional support for 'frequency' a la LDCs
     */
    sprintf(data->format,"phase");
    
    data->t0   = malloc(sizeof(double)*Nmax);
    data->tgap = malloc(sizeof(double)*Nmax);
    
    for(int j=0; j<Nmax; j++)
    {
        data->t0[j]   = 0.0;
        data->tgap[j] = 0.0;
    }
    
    data->T        = 62914560.0; /* two "mldc years" at 15s sampling */
    data->N        = 1024;
    data->NP       = 8; //default includes fdot
    data->Nchannel = 2; //1=X, 2=AE
    data->DMAX     = DMAX_default;//maximum number of sources
    
    data->cseed = 150914;
    data->nseed = 151226;
    data->iseed = 151012;
    
    
    
    //Specifying the expected options
    static struct option long_options[] =
    {
        /* These options set a flag. */
        {"samples",   required_argument, 0, 0},
        {"padding",   required_argument, 0, 0},
        {"duration",  required_argument, 0, 0},
        {"segments",  required_argument, 0, 0},
        {"sources",   required_argument, 0, 0},
        {"orbit",     required_argument, 0, 0},
        {"fmin",      required_argument, 0, 0},
        {"links",     required_argument, 0, 0},
        {"chain-file",required_argument, 0, 0},
        {"noise-file",required_argument, 0, 0},
        {"match",     required_argument, 0, 0},
        {"catalog",   required_argument, 0, 0},
        {"Tcatalog",  required_argument, 0, 0},
        {"Nmode",     required_argument, 0, 0},
        {"thin",      required_argument, 0, 0},
        
        /* These options don’t set a flag.
         We distinguish them by their indices. */
        {"help",        no_argument, 0,'h'},
        {"verbose",     no_argument, 0,'v'},
        {"frac-freq",   no_argument, 0, 0 },
        {"f-double-dot",no_argument, 0, 0 },
        {0, 0, 0, 0}
    };
    
    int opt=0;
    int long_index=0;
    
    //Loop through argv string and pluck out arguments
    while ((opt = getopt_long_only(argc, argv,"apl:b:", long_options, &long_index )) != -1)
    {
        switch (opt)
        {
                
            case 0:
                if(strcmp("samples",     long_options[long_index].name) == 0) data->N    = atoi(optarg);
                if(strcmp("padding",     long_options[long_index].name) == 0) data->qpad = atoi(optarg);
                if(strcmp("duration",    long_options[long_index].name) == 0) data->T    = (double)atof(optarg);
                if(strcmp("fmin",        long_options[long_index].name) == 0) data->fmin = (double)atof(optarg);
                if(strcmp("Tcatalog",    long_options[long_index].name) == 0) *Tcatalog      = (double)atof(optarg);
                if(strcmp("Nmode",       long_options[long_index].name) == 0) *NMODE         = (size_t)atoi(optarg);
                if(strcmp("thin",        long_options[long_index].name) == 0) *NTHIN         = (size_t)atoi(optarg);
                if(strcmp("f-double-dot",long_options[long_index].name) == 0) data->NP   = 9;
                if(strcmp("sources",     long_options[long_index].name) == 0)
                {
                    data->DMAX    = atoi(optarg);
                    flags->DMAX       = atoi(optarg);
                }
                if(strcmp("frac-freq",   long_options[long_index].name) == 0)
                {
                    sprintf(data->format,"frequency");
                }
                if(strcmp("orbit", long_options[long_index].name) == 0)
                {
                    checkfile(optarg);
                    flags->orbit = 1;
                    sprintf(orbit->OrbitFileName,"%s",optarg);
                }
                if(strcmp("noise-file", long_options[long_index].name) == 0)
                {
                    checkfile(optarg);
                    sprintf(flags->noiseFile,"%s",optarg);
                    flags->simNoise = 0;
                }
                if(strcmp("chain-file", long_options[long_index].name) == 0)
                {
                    checkfile(optarg);
                    sprintf(data->fileName,"%s",optarg);
                }
                if(strcmp("catalog",long_options[long_index].name) == 0)
                {
                    flags->catalog = 1;
                    sprintf(flags->catalogFile,"%s",optarg);
                }
                
                if(strcmp("match", long_options[long_index].name) == 0)
                {
                    data->pmax = atof(optarg);
                }
                if(strcmp("links",long_options[long_index].name) == 0)
                {
                    int Nlinks = (int)atoi(optarg);
                    switch(Nlinks)
                    {
                        case 4:
                            data->Nchannel=1;
                            break;
                        case 6:
                            data->Nchannel=2;
                            break;
                        default:
                            fprintf(stderr,"Requested umber of links (%i) not supported\n",Nlinks);
                            fprintf(stderr,"Use --links 4 for X (Michelson) data\n");
                            fprintf(stderr,"    --links 6 for AE data\n");
                            exit(1);
                    }
                }
                break;
            case 'h' :
                print_usage_catalog();
                exit(EXIT_FAILURE);
                break;
            case 'v' : flags->verbose = 1;
                break;
            default: print_usage_catalog();
                exit(EXIT_FAILURE);
        }
    }
    
    //check if catalog is supplied so is corresponding duration
    if(flags->catalog && *Tcatalog<0)
    {
        fprintf(stderr,"Use of --catalog flag requires --Tcatalog argument\n");
        fprintf(stderr,"Supply observing time for input catalog or remove the option\n");
        exit(1);
    }
    
    //pad data
    data->N += 2*data->qpad;
    data->fmin -= data->qpad/data->T;
    
    
    // copy command line args to other data structures
    data->NT = flags->NT;
    for(int j=0; j<flags->NT; j++)
    {
        data->t0[j]   = data->t0[0] + j*(data->T + data->tgap[0]);
        data->tgap[j] = data->tgap[0];
    }
    
    //map fmin to nearest bin
    data->fmin = floor(data->fmin*data->T)/data->T;
    
    //Print command line
    char filename[128];
    sprintf(filename,"catalog_%i.sh",data->DMAX);
    FILE *out = fopen(filename,"w");
    fprintf(out,"#!/bin/sh\n\n");
    for(opt=0; opt<argc; opt++) fprintf(out,"%s ",argv[opt]);
    fprintf(out,"\n\n");
    fclose(out);
    
    //Print version control
    FILE *runlog = fopen("gb_catalog.log","w");
    print_version(runlog);
    
    fclose(runlog);
}

static int gaussian_mixture_model_wrapper(double **ranges, struct Flags *flags, struct Entry *entry, char *outdir, size_t NP, size_t NMODE, size_t NTHIN, gsl_rng *seed, double *BIC)
{
    fprintf(stdout,"Event %s, NMODE=%i\n",entry->name,(int)NMODE);
    
    // number of samples
    size_t NMCMC = entry->I;
    
    // number of EM iterations
    size_t NSTEP = 100;
    
    // thin chain
    NMCMC /= NTHIN;
    
    struct Sample **samples = malloc(NMCMC*sizeof(struct Sample*));
    for(size_t n=0; n<NMCMC; n++)
    {
        samples[n] = malloc(sizeof(struct Sample));
        samples[n]->x = gsl_vector_alloc(NP);
        samples[n]->p = gsl_vector_alloc(NMODE);
        samples[n]->w = gsl_vector_alloc(NMODE);
    }
    
    // covariance matrices for different modes
    struct MVG **modes = malloc(NMODE*sizeof(struct MVG*));
    for(size_t n=0; n<NMODE; n++)
    {
        modes[n] = malloc(sizeof(struct MVG));
        alloc_MVG(modes[n],NP);
    }
    
    // Logistic mapping of samples onto R
    double y;
    double pmin,pmax;
    gsl_vector *y_vec = gsl_vector_alloc(NMCMC);
    
    /* parse chain file */
    gsl_vector **params = malloc(NP*sizeof(gsl_vector *));
    for(size_t n=0; n<NP; n++) params[n] = gsl_vector_alloc(NMCMC);
    double value[NP];
    char *column;
    for(size_t i=0; i<NMCMC; i++)
    {
        value[0] = entry->source[i*NTHIN]->f0;
        value[1] = entry->source[i*NTHIN]->costheta;
        value[2] = entry->source[i*NTHIN]->phi;
        value[3] = log(entry->source[i*NTHIN]->amp);
        //value[4] = acos(entry->source[i*NTHIN]->cosi);
        value[4] = entry->source[i*NTHIN]->cosi;
        value[5] = entry->source[i*NTHIN]->psi;
        value[6] = entry->source[i*NTHIN]->phi0;
        if(NP>7)
            value[7] = entry->source[i*NTHIN]->dfdt;
        if(NP>8)
            value[8] = entry->source[i*NTHIN]->d2fdt2;
        
        for(size_t n=0; n<NP; n++)
        {
            //gsl_vector_set(samples[i]->x,n,value[n]);
            gsl_vector_set(params[n],i,value[n]);
        }
    }
    
    /* Get max and min for each parameter
     for(size_t n=0; n<NP; n++)
     {
     int err = 0;
     double *temp = malloc(10*sizeof(double));
     err = gsl_sort_vector_smallest(temp, 10, params[n]);
     pmin = temp[0] - (temp[9]-temp[0]); //pad min to avoid infs in mapping
     
     err = gsl_sort_vector_largest(temp, 10, params[n]);
     pmax = temp[0] + (temp[0]-temp[9]); //pad max to avoid infs in mapping
     
     // copy max and min into each MVG structure
     for(size_t k=0; k<NMODE; k++)
     {
     gsl_matrix_set(modes[k]->minmax,n,0,pmin);
     gsl_matrix_set(modes[k]->minmax,n,1,pmax);
     }
     
     }*/
    
    /* Use priors to set min and max of each parameter*/
    for(size_t n=0; n<NP; n++)
    {
        // copy max and min into each MVG structure
        for(size_t k=0; k<NMODE; k++)
        {
            gsl_matrix_set(modes[k]->minmax,n,0,ranges[n][0]);
            gsl_matrix_set(modes[k]->minmax,n,1,ranges[n][1]);
        }
    }
    
    
    /* map params to R with logit function */
    for(size_t n=0; n<NP; n++)
    {
        pmin = gsl_matrix_get(modes[0]->minmax,n,0);
        pmax = gsl_matrix_get(modes[0]->minmax,n,1);
        logit_mapping(params[n], y_vec, pmin, pmax);
        
        for(size_t i=0; i<NMCMC; i++)
        {
            y = gsl_vector_get(y_vec,i);
            gsl_vector_set(samples[i]->x,n,y);
        }
    }
    
    /* The main Gaussian Mixture Model with Expectation Maximization function */
    double logL;
    if(GMM_with_EM(modes,samples,NMCMC,NSTEP,seed,&logL,BIC)) return 1;
    
    /* Write GMM results to binary for pick up by other processes */
    char filename[BUFFER_SIZE];
    sprintf(filename,"%s/%s_gmm.bin",outdir,entry->name);
    FILE *fptr = fopen(filename,"wb");
    fwrite(&NMODE, sizeof NMODE, 1, fptr);
    for(size_t n=0; n<NMODE; n++) write_MVG(modes[n],fptr);
    fclose(fptr);
    
    /* print 1D PDFs and 2D contours of GMM model */
    if(flags->verbose) print_model(modes, samples, NMCMC, logL, *BIC, NMODE);
    
    /* clean up */
    for(size_t n=0; n<NMCMC; n++)
    {
        gsl_vector_free(samples[n]->x);
        gsl_vector_free(samples[n]->p);
        gsl_vector_free(samples[n]->w);
        free(samples[n]);
    }
    free(samples);
    
    // covariance matrices for different modes
    for(size_t n=0; n<NMODE; n++) free_MVG(modes[n]);
    free(modes);
    
    return 0;
}
