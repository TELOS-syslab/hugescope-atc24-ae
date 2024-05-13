#include "ss_work.h"
#include <linux/slab.h>
#include <linux/random.h>

#define bias 100000
#define SVMPFNHASHMOD 9999997
#define SVM_LABELLIMIT 10
#define SVM_DIMENSION 2
#define TRAIN_SET 10000

void *my_malloc(long t)
{
	void *ptr;
	ptr = (void *)kmalloc(t, GFP_KERNEL);
	if (ptr == NULL) {
		printk("erro,out of memery!\n");
		return NULL;
	} else
		return ptr;
}

void init_svm(problem_parameter *pp)
{
	pp->w = (long *)my_malloc(SVM_DIMENSION * sizeof(long));
	pp->b = (long *)my_malloc(sizeof(long));

	pp->threadhold = SVM_DEFAULT_THREADHOLD;

	*(pp->b) = 0;
	pp->w[0] = bias;
	pp->w[1] = 3 * bias;

	return;
}

void init_Prob_Para(problem_parameter *prob_para, long **data_vector,
		    long *label, int dimension, int size, long C,
		    long tolerance)
{
	int jj;
	prob_para->data_vector = data_vector;
	prob_para->label = label;
	prob_para->dimension = dimension;
	prob_para->size = size;
	prob_para->C = C;
	prob_para->tolerance = tolerance;
	prob_para->Erroi = (long *)my_malloc(size * sizeof(long));
	prob_para->alpha = (long *)my_malloc(size * sizeof(long));

	for (jj = 0; jj < size; jj++) {
		prob_para->Erroi[jj] = 0;
		prob_para->alpha[jj] = 0;
	}
}

long dot_linear(long *x1, long *x2, int m)
{
	long y = 0, kk = 0;
	for (kk = 0; kk < m; kk++, x1++, x2++)
		y += (*x1) * (*x2) / bias;
	//printf("y:::::%ld\n",y);
	return y;
}

int examineExample(int i1, problem_parameter *prob_para)
{
	long y1, alpha1, E1, r1;
	y1 = prob_para->label[i1];
	alpha1 = prob_para->alpha[i1];

	if (alpha1 > 0 && alpha1 < prob_para->C)
		E1 = prob_para->Erroi[i1];
	else
		E1 = learned_fun(prob_para->w, *prob_para->b,
				 prob_para->data_vector[i1],
				 prob_para->dimension) -
		     y1;

	r1 = y1 * E1 / bias;
	//printf("%ld %ld %ld  \n",r1,y1,alpha1);

	if ((r1 < -prob_para->tolerance && alpha1 < prob_para->C) ||
	    (r1 > prob_para->tolerance && alpha1 > 0)) {
		//printf("%ld %ld %ld  \n",r1,y1,alpha1);
		if (try_argmaxE1_E2(i1, E1, prob_para))
			return 1;
		else if (try_iter_non_boundary(i1, prob_para))
			return 1;
		else if (try_iter_all(i1, prob_para))
			return 1;
	}

	return 0; // all failed
}

long learned_fun(long *w, long b, long *x, int m)
{
	int kk;
	long y = 0;

	for (kk = 0; kk < m; kk++) {
		y += w[kk] * x[kk] / bias;
	}

	//printf("y:::: %ld\n", y);
	return y - b;
}

int try_argmaxE1_E2(int i1, long E1, problem_parameter *prob_para)
{
	int k, i2;
	long tmax;
	long E2, temp;

	//printf("----enter1\n");
	for (i2 = -1, tmax = 0, k = 0; k < prob_para->size; k++)
		if (prob_para->alpha[k] > 0 &&
		    prob_para->alpha[k] < prob_para->C) {
			E2 = prob_para->Erroi[k];
			temp = E1 - E2;
			temp = (temp < 0 ? -temp : temp);
			if (temp > tmax) {
				tmax = temp;
				i2 = k;
			}
		}

	if (i2 >= 0) {
		if (takeStep(i1, i2, prob_para))
			return 1;
	}
	return 0;
}

int try_iter_non_boundary(int i1, problem_parameter *prob_para)
{
	int k, k0;
	int i2;
	//printf("----enter2\n");
	get_random_bytes(&k0, 4);
	if (k0 < 0)
		k0 = -k0;
	k0 = k0 % prob_para->size;
	for (k = k0; k < prob_para->size + k0; k++) {
		i2 = k % prob_para->size;
		if (prob_para->alpha[i2] > 0 &&
		    prob_para->alpha[i2] < prob_para->C) {
			if (takeStep(i1, i2, prob_para))
				return 1;
		}
	}
	return 0;
}
int try_iter_all(int i1, problem_parameter *prob_para)
{
	int k, k0;
	int i2;
	//printf("----enter3\n");
	get_random_bytes(&k0, 4);
	if (k0 < 0)
		k0 = -k0;
	k0 = k0 % prob_para->size;
	for (k = k0; k < prob_para->size + k0; k++) {
		//printf("kkkkkkkkkkkkkkk:%d\n",rand());
		i2 = k % prob_para->size;
		if (takeStep(i1, i2, prob_para))
			return 1;
	}

	return 0;
}

int takeStep(int i1, int i2, problem_parameter *prob_para)
{
	long y1, y2, s;
	long alpha1, alpha2;
	long a1, a2;
	long E1, E2, L, H, k11, k22, k12, eta, Lobj, Hobj;
	long delta_b;
	long tolerance;
	long gamma;
	long c1, c2;
	long t, t1, t2;
	long b1, b2, bnew;
	int ii, kk;

	tolerance = prob_para->tolerance;
	if (i1 == i2)
		return 0;
	alpha1 = prob_para->alpha[i1];
	y1 = prob_para->label[i1];

	if (alpha1 > 0 && alpha1 < prob_para->C)
		E1 = prob_para->Erroi[i1];
	else
		E1 = learned_fun(prob_para->w, *prob_para->b,
				 prob_para->data_vector[i1],
				 prob_para->dimension) -
		     y1;

	alpha2 = prob_para->alpha[i2];
	y2 = prob_para->label[i2];
	if (alpha2 > 0 && alpha2 < prob_para->C)
		E2 = prob_para->Erroi[i2];
	else
		E2 = learned_fun(prob_para->w, *prob_para->b,
				 prob_para->data_vector[i2],
				 prob_para->dimension) -
		     y2;

	s = y1 * y2 / bias;

	// compute L,H
	if (y1 == y2) {
		gamma = alpha1 + alpha2;
		if (gamma > prob_para->C) {
			L = gamma - prob_para->C;
			H = prob_para->C;
		} else {
			L = 0;
			H = gamma;
		}
	} else {
		gamma = alpha1 - alpha2;
		if (gamma > 0) {
			L = 0;
			H = prob_para->C - gamma;
		} else {
			L = -gamma;
			H = prob_para->C;
		}
	}

	// compute eta
	k11 = dot_linear(prob_para->data_vector[i1], prob_para->data_vector[i1],
			 prob_para->dimension);
	k12 = dot_linear(prob_para->data_vector[i1], prob_para->data_vector[i2],
			 prob_para->dimension);
	k22 = dot_linear(prob_para->data_vector[i2], prob_para->data_vector[i2],
			 prob_para->dimension);
	eta = 2 * k12 - k11 - k22;
	//printf("eta:: %ld\n",eta);
	//printf("s, L, H, E1, E2:  %ld %ld %ld %ld %ld\n", s, L, H, E1, E2);

	if (eta < 0) {
		a2 = alpha2 + y2 * (E2 - E1) / eta;
		if (a2 < L)
			a2 = L;
		else if (a2 > H)
			a2 = H;
	} else {
		// compute Lobj,Hobj;
		c1 = eta / 2;
		c2 = y2 * (E2 - E1) - eta * alpha2;
		Lobj = c1 * L * L + c2 * L;
		Hobj = c1 * H * H + c2 * H;
		if (Lobj > Hobj + tolerance)
			a2 = L;
		else if (Lobj < Hobj - tolerance)
			a2 = H;
		else
			a2 = alpha2;
	}
	//printf("a2, alpha2, tolerance: %ld %ld %ld %ld\n",a2, alpha2, tolerance, tolerance*(a2+alpha2+tolerance));
	if (((a2 - alpha2) > 0 ? a2 - alpha2 : alpha2 - a2) * bias <
	    tolerance * (a2 + alpha2 + tolerance))
		return 0;

	//printf("out\n");
	a1 = alpha1 - s * (a2 - alpha2) / bias;
	//printf("a1, a2:  %ld %ld\n", a1, a2);
	if (a1 < 0) {
		a2 += s * a1 / bias;
		a1 = 0;
	} else if (a1 > prob_para->C) {
		t = a1 - prob_para->C;
		a2 += s * t / bias;
		a1 = prob_para->C;
	}

	//printf("a1, a2:  %ld %ld\n", a1, a2);

	// update threshold
	if (1) {
		if (a1 > 0 && a1 < prob_para->C)
			bnew = *prob_para->b + E1 +
			       y1 / bias * (a1 - alpha1) * k11 / bias +
			       y2 / bias * (a2 - alpha2) * k12 / bias;

		else {
			if (a2 > 0 && a2 < prob_para->C)
				bnew = *prob_para->b + E2 +
				       y1 / bias * (a1 - alpha1) * k12 / bias +
				       y2 / bias * (a2 - alpha2) * k22 / bias;

			else {
				b1 = *prob_para->b + E1 +
				     y1 / bias * (a1 - alpha1) * k11 / bias +
				     y2 / bias * (a2 - alpha2) * k12 / bias;
				b2 = *prob_para->b + E2 +
				     y1 / bias * (a1 - alpha1) * k12 / bias +
				     y2 / bias * (a2 - alpha2) * k22 / bias;
				bnew = (b1 + b2) / 2;
				//printf("bnew:  %ld\n", bnew);
			}
		}
		delta_b = bnew - *prob_para->b;
		*prob_para->b = bnew;
		//printf("delta_b, bnew:   %ld, %ld\n", delta_b, bnew);
	}

	// update weight vector
	// for linear kenner
	if (1) {
		t1 = y1 * (a1 - alpha1) / bias;
		t2 = y2 * (a2 - alpha2) / bias;
		//printf("t1, t2:   %ld  %ld\n", t1,t2);
		for (ii = 0; ii < prob_para->dimension; ii++) {
			//printf("woc???\n");
			prob_para->w[ii] =
				t1 * prob_para->data_vector[i1][ii] / bias +
				t2 * prob_para->data_vector[i2][ii] / bias +
				prob_para->w[ii];
			//printf("w  :   %ld\n", prob_para->w[ii]);
		}
	}

	if (1) {
		t1 = y1 * (a1 - alpha1) / bias;
		t2 = y2 * (a2 - alpha2) / bias;
		for (kk = 0; kk < prob_para->dimension; kk++) {
			if (0 < prob_para->alpha[kk] &&
			    prob_para->alpha[kk] < prob_para->C) {
				prob_para->Erroi[kk] =
					t1 *
						dot_linear(
							prob_para
								->data_vector[i1],
							prob_para
								->data_vector[kk],
							prob_para->dimension) /
						bias +
					t2 *
						dot_linear(
							prob_para
								->data_vector[i2],
							prob_para
								->data_vector[kk],
							prob_para->dimension) /
						bias -
					delta_b;

				//printf("errori  :  %ld\n", prob_para->Erroi[kk]);
			}
			prob_para->Erroi[i1] = 0;
			prob_para->Erroi[i2] = 0;
		}
	}

	prob_para->alpha[i1] = a1;
	prob_para->alpha[i2] = a2;
	return 1;
}

void svm_train(problem_parameter *prob_para)
{
	int examineall = 1;
	int numChanged = 0;
	int k;

	*prob_para->b = 0;
	prob_para->w[0] = 0;
	prob_para->w[1] = 0;

	while (numChanged || examineall) {
		numChanged = 0;

		if (examineall) {
			//printf("goes flag 1 \n");
			for (k = 0; k < prob_para->size; k++) {
				//printf("%d\n", k);
				numChanged += examineExample(k, prob_para);
			}
			//printf("numChanged %d examineall %d \n",numChanged,examineall );
		} else {
			for (k = 0; k < prob_para->size; k++) {
				if (prob_para->alpha[k] != 0 &&
				    prob_para->alpha[k] != prob_para->C)
					numChanged +=
						examineExample(k, prob_para);
			}
		}
		if (examineall)
			examineall = 0;
		else if (numChanged == 0)
			examineall = 1;

		//return;
	}

	// free the space // not free the alpha space ,because of later version
	kfree(prob_para->Erroi);
	prob_para->Erroi = NULL;
}

void svm_train_data(problem_parameter *pp, int lable_bdy,
		    struct page_node *pages, int nums, int watch)
{
	int i;
	int j;
	int size = 0;
	long *data_vector;
	long *label;
	int kk = 0;
	long **ptarray;
	int tra;
	int r, w, l;

	size = TRAIN_SET;
	data_vector = (long *)my_malloc(SVM_DIMENSION * size * sizeof(long));
	label = (long *)my_malloc(size * sizeof(long));

	tra = watch * 2 / 3;

	kk = 0;
	for (i = 0; i < nums; i++) {
		if (!pages[i].p)
			continue;
		r = 0;
		w = 0;
		l = 0;
		for (j = 0; j < tra; ++j) {
			if (pages[i].acc_map[j] == 1)
				r++;
			if (pages[i].acc_map[j] == 2)
				w++;
		}
		for (j = tra; j < watch; ++j) {
			if (pages[i].acc_map[j] == 1)
				l++;
			if (pages[i].acc_map[j] == 2)
				l += 3;
		}
		if (kk == size)
			kk = size - 1;
		label[kk] = (l > lable_bdy) ? 1 : -1;
		label[kk] *= bias;
		data_vector[kk * SVM_DIMENSION] = r * bias;
		data_vector[kk * SVM_DIMENSION + 1] = w * bias;
		kk++;
	}
	size = kk;
	printk("train_set_num: %d\n", kk);
	ptarray = (long **)my_malloc(size * sizeof(long *));
	for (i = 0; i < size; i++)
		ptarray[i] = data_vector + i * SVM_DIMENSION;

	init_Prob_Para(pp, ptarray, label, SVM_DIMENSION, size, bias,
		       bias / 1000);
}

int svm_test(problem_parameter *prob_para, struct page_node *pages, int nums,
	     int watch, int *acc, int *tot)
{
	int i;
	int j;
	int tra;
	int r, w, l;
	long value;
	int label_bdy;
	int ret;

	int low;
	int high;

	low = high = 0;
	tra = watch / 3 * 2;
	(*acc) = 0;
	(*tot) = 0;

	label_bdy = prob_para->threadhold;

	for (i = 0; i < nums; i++) {
		if (!pages[i].p)
			continue;
		r = 0;
		w = 0;
		l = 0;

		for (j = 0; j < tra; ++j) {
			if (pages[i].acc_map[j] == 1)
				r++;
			if (pages[i].acc_map[j] == 3)
				w++;
		}
		for (j = tra; j < watch; ++j) {
			if (pages[i].acc_map[j] == 1)
				l++;
			if (pages[i].acc_map[j] == 3)
				l += 3;
		}

		l = (l >= label_bdy) ? 1 : -1;
		value = r * prob_para->w[0] + w * prob_para->w[1] -
			(*prob_para->b);

		//if((value>0&&l==1)||(value<=0&&l==-1))
		//	(*acc)++;

		(*tot)++;

		if (value > 0 && l == -1) {
			low++;
		} else if (value <= 0 && l == 1) {
			high++;
		} else {
			(*acc)++;
		}
	}
	ret = (low > high) ? 0 : 1;
	return ret;
}

bool accuracy_checker(struct vm_context *vmc)
{
	int acc, tot;
	int acc_rate;
	int ret;
	printk("svm_test begin\n");
	ret = svm_test(&(vmc->pp), vmc->busy_pages, vmc->page_num,
		       SVM_TRACE_LEN, &acc, &tot);
	printk("svm_test end\n");
	acc_rate = acc * 100 / tot;

	if (acc_rate > 90)
		return true;
	//如果准确率过低，就根据错误分类的结果，增高/降低阈值
	//若大部分错误分类是因为阈值低导致的，增加阈值
	//反之，降低阈值
	if (ret) {
		vmc->pp.threadhold = vmc->pp.threadhold * 6 / 5;
	} else {
		vmc->pp.threadhold = vmc->pp.threadhold * 4 / 5;
	}

	return false;
}
