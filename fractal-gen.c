/*
 * fractal-gen - Generate iteration-based fractals in PNM format
 * Copyright (c) 2015 David Phillips <dbphillipsnz@gmail.com>
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "fractal-gen.h"
#include "generator.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libgen.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

static struct section_generator generators[] = {
	{ "mandelbrot-gen" , &generate_mandelbrot_section },
	{ "burning-ship-gen" , &generate_burning_ship_section },
	{ "burning-ship-lattice-gen" , &generate_burning_ship_lattice_section }
};


int main(int argc, char **argv)
{
	unsigned long x = 0;
	unsigned long y = 0;
	unsigned long i = 0;
	double ram_nice = 0.f; /* Forecast RAM usage, divided down to < 1024 */
	char* ram_unit = NULL; /* Unit for ram_nice */
	char* bname = NULL;
	data_section* sections = NULL;
	generator_func generator = NULL;

	/* who are we? */
	argv0 = argv[0];

	/* Select correct generator for the fractal type */
	bname = basename(argv[0]);
	generator = select_generator(bname);

	if (generator == NULL)
	{
		fprintf(stderr, "Don't call this directly, call a symlink to me\n");
		return EXIT_FAILURE;
	}

	if (!args_parse_okay(argc, argv))
	{
		show_help();
		return EXIT_FAILURE;
	}

	/* Allocate memory for sections */
	if ((sections = malloc(sizeof(data_section)*cores)) == NULL)
	{
		perror("malloc");
		return EXIT_FAILURE;
	}

	ram_nice = (size*size)/clust_total;
	if (ram_nice < 1024)
		ram_unit = "B";
	else if (ram_nice < 1024*1024)
		ram_nice /= 1024, ram_unit = "KiB";
	else if (ram_nice < 1024*1024*1024)
		ram_nice /= (1024*1024), ram_unit = "MiB";
	else
		ram_nice /= (1024*1024*1024), ram_unit = "GiB";

	fprintf(stderr,
		"Forecast resource use:\n"
		" Threads: %d\n"
		" RAM    : ~%.4f %s\n",
		cores,
		ram_nice,
		ram_unit);
	/* Spawn all the threads! Something something interlacing */
	for (i = 0; i < cores; i++)
	{
		/* A bit complex, icky, will document later */
		if (i < (size%cores))
			x = (size/cores)+1;
		else
			x = (size/cores);

		x *= size;
		x = ceilf((double)x/clust_total);

		if ((sections[i].data = malloc(x)) == NULL)
		{
			fprintf(stderr, "\nmalloc of %lu bytes failed\n", x);
			perror("malloc");

			/* Free already allocated chunks of memory */
			i--;
			while(i-- + 1)
				free(sections[i].data);

			free(sections);
			return EXIT_FAILURE;
		}
		sections[i].core = i;
		sections[i].datasize = x;
		fprintf(stderr, " -> Thread %lu\r", i);
		pthread_create(&sections[i].thread, NULL, generator, &(sections[i]));
	}

	while((x = sections[0].idx) < sections[0].datasize)
	{
		fprintf(stderr, "Thread 0: %.4f%%\r", 100.f*(double)x/sections[0].datasize );
		sleep(1);
	}

	/* Wait for each thread to complete */
	for (i = 0; i < cores; i++)
		pthread_join(sections[i].thread, NULL);


	/* Output PGM Header */
	printf("P5\n%d\n%d\n255\n",size/clust_total,size);

	/* Vomit the data segments back onto the screen, deinterlacing
	 * TO DO: look at fwrite performance benefits over putchar */
	for (y = 0; y < size; y++)
		for (x = 0; x < size/clust_total; x++)
			putchar(sections[y%cores].data[(y/cores)*(size/clust_total) + x]);

	fprintf(stderr, "\nDone\n");

	/* Free the memory we allocated for point data */
	for (i = 0; i < cores; i++)
		free(sections[i].data);

	free(sections);
	return 0;
}


bool args_parse_okay(int argc, char **argv)
{
	char opt = '\0';

	/* first things first: preload default or initial values */
	size = 0;
	iterat = 0;
	power = 2;
	cores = sysconf(_SC_NPROCESSORS_ONLN);
	thread_mult = 1;
	clust_id = 0;
	clust_total = 1;

	while ( (opt = getopt(argc, argv, "s:i:e:c:t:N:T")) != -1 )
	{
		switch (opt)
		{
			case 's': size = atoi(optarg); break;
			case 'i': iterat = atoi(optarg); break;
			case 'e': power = atoi(optarg); break;
			case 'c': cores = atoi(optarg); break;
			case 't': thread_mult = atoi(optarg); break;
			case 'N': clust_id = atoi(optarg); break;
			case 'T': clust_total = atoi(optarg); break;
			/* redundant case for '?', but explicitness is best */
			case '?':
			default:
				return false;
				break;
		}
	}

	/* Extend number of threads to multiplier value */
	cores *= thread_mult;

	/* Interlacing is column-based, can't have more workers than columns */
	if (cores > size)
	{
		cores = size;
		fprintf(stderr, "WARN: Capping number of threads to image size (%d)\n", cores);
	}

	if (size % clust_total != 0)
	{
		fprintf(stderr, "ERROR: image size must be an exact multiple of clust_total\n");
		return false;
	}

	if (size <= 0)
	{
		fprintf(stderr, "size should be positive\n");
		return false;
	}

	if (iterat <= 0)
	{
		fprintf(stderr, "iteration count should be positive\n");
		return false;
	}

	if (cores <= 0)
	{
		fprintf(stderr, "core counts should be positive\n");
		return false;
	}
	return true;
}


generator_func select_generator(const char* name)
{
	unsigned long i = 0;
	for (i = 0; i < sizeof(generators)/sizeof(struct section_generator); i++)
		if (strcmp(name, generators[i].executable_name) == 0)
			return generators[i].generator;

	return NULL;
}

void show_help()
{
	fprintf(stderr,
			"%s -s size -i iterat [-e exponent]\n"
			"        [-c cores] [-t thread_multiplier]\n"
			"        [-N cluster-id -T cluster-total]\n",
			argv0);
}
