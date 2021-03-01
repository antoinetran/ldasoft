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

/**
 @file GalacticBinaryCatalog.h
 \brief Codes for postprocessing GBMCMC results and formatting for catalog production
 
 */


#ifndef GalacticBinaryCatalog_h
#define GalacticBinaryCatalog_h

#include <stdio.h>

/*!
 \brief Prototype structure for catalog of detected sources.
 
 Contains size of the recovered catalog and structures for each sources.
*/
struct Catalog
{
    int N; //!<number of discrete sources in catalog
    struct Entry **entry; //!<discrete catalog entries
};

/*!
 \brief Prototype structure for individual source entries in the catalog.
 
 Contains metadata describing/labeling the source,
 and the full posterior reconstruction (parameters, waveforms, etc.).
*/
struct Entry
{
    int I;                  //!<number of chain samples
    char name[128];         //!<source name
    char parent[128];       //!<source parent name
    char path[1024];        //!<path to catalog entry
    struct Source **source; //!<source structure contains parameters, defined in GalacticBinary.h
    double *match;          //!<match between sample and ref. source
    double *distance;       //!<metric distance between sample and ref. source
    double evidence;        //!<source evidence
    double SNR;             //!<reference SNR of source
    int i;                  //!<sample containing med. freq.
    struct GMM *gmm;        //!<Gaussian Mixture Model representation of posterior.
};

/**
 \brief Allocates memory for catalog entry (i.e. indivudual source) and initializes with input source.
 */
void alloc_entry(struct Entry *entry, int IMAX);

/**
 \brief Allocates memory for new catalog entry (i.e. individual source) without initializing contents.
 */
void create_empty_source(struct Catalog *catalog, int NFFT, int Nchannel, int NP);

/**
 \brief Allocates memory for catalog entry (i.e. indivudual source) and initializes with input `sample`.
 */
void create_new_source(struct Catalog *catalog, struct Source *sample, struct Noise *noise, int IMAX, int NFFT, int Nchannel, int NP);

/**
\brief Adds input `sample` to existing catalog entry and increments counters.
*/
void append_sample_to_entry(struct Entry *entry, struct Source *sample, int IMAX, int NFFT, int Nchannel, int NP);

/**
 \brief Wrapper for using functions in GMM_with_EM.c to represent posterior samples of `entry` as a Gaussian Mixture Model.
 */
int gaussian_mixture_model_wrapper(double **ranges, struct Flags *flags, struct Entry *entry, char *outdir, size_t NP, size_t NMODE, size_t NTHIN, gsl_rng *seed, double *BIC);


#endif /* GalacticBinaryCatalog_h */
