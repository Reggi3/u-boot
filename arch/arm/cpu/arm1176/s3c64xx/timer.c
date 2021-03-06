/*
 * Copyright (C) 2009 Samsung Electronics
 * Heungjun Kim <riverful.kim@samsung.com>
 * Inki Dae <inki.dae@samsung.com>
 * Minkyu Kang <mk7.kang@samsung.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/pwm.h>
#include <asm/arch/clk.h>
#include <div64.h>
#include <pwm.h>

DECLARE_GLOBAL_DATA_PTR;

unsigned long get_current_tick(void);

static inline struct s3c_timer *s3c_get_base_timer(void)
{
	return (struct s3c_timer *)samsung_get_base_timer();
}

int timer_init(void)
{
	/* PWM Timer 4 */
	pwm_init(4, MUX_DIV_2, 0);
	pwm_config(4, 0, 0);
	pwm_enable(4);

	reset_timer_masked();

	return 0;
}

/*
 * timer without interrupts
 */
unsigned long get_timer(unsigned long base)
{
	return get_timer_masked() - base;
}

/* delay x useconds */
void __udelay(unsigned long usec)
{
	struct s3c_timer *const timer = s3c_get_base_timer();
	unsigned long tmo, tmp, count_value;

	count_value = readl(&timer->tcntb4);

	if (usec >= 1000) {
		/*
		 * if "big" number, spread normalization
		 * to seconds
		 * 1. start to normalize for usec to ticks per sec
		 * 2. find number of "ticks" to wait to achieve target
		 * 3. finish normalize.
		 */
		tmo = usec / 1000;
		tmo *= (CONFIG_SYS_HZ * count_value);
		tmo /= 1000;
	} else {
		/* else small number, don't kill it prior to HZ multiply */
		tmo = usec * CONFIG_SYS_HZ * count_value;
		tmo /= (1000 * 1000);
	}

	/* get current timestamp */
	tmp = get_current_tick();

	/* if setting this fordward will roll time stamp */
	/* reset "advancing" timestamp to 0, set lastinc value */
	/* else, set advancing stamp wake up time */
	if ((tmo + tmp + 1) < tmp)
		reset_timer_masked();
	else
		tmo += tmp;

	/* loop till event */
	while (get_current_tick() < tmo)
		;	/* nop */
}

void reset_timer_masked(void)
{
	struct s3c_timer *const timer = s3c_get_base_timer();

	/* reset time */
	gd->arch.lastinc = readl(&timer->tcnto4);
	gd->arch.tbl = 0;
}

ulong get_timer_masked(void)
{
	struct s3c_timer *const timer = s3c_get_base_timer();
	unsigned long count_value = readl(&timer->tcntb4);

	return get_current_tick() / count_value;
}

unsigned long get_current_tick(void)
{
	struct s3c_timer *const timer = s3c_get_base_timer();
	unsigned long now = readl(&timer->tcnto4);
	unsigned long count_value = readl(&timer->tcntb4);

	if (gd->arch.lastinc >= now)
		gd->arch.tbl += gd->arch.lastinc - now;
	else
		gd->arch.tbl += gd->arch.lastinc + count_value - now;

	gd->arch.lastinc = now;

	return gd->arch.tbl;
}

unsigned long long get_ticks(void)
{
	return get_timer(0);
}

unsigned long get_tbclk(void)
{
	return CONFIG_SYS_HZ;
}
