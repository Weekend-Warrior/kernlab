#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <float.h>
#include <string.h>
#include <stdarg.h>
#include "bsvm.h"
typedef float Qfloat;
typedef signed char schar;
#ifndef min
template <class T> inline T min(T x,T y) { return (x<y)?x:y; }
#endif
#ifndef max
template <class T> inline T max(T x,T y) { return (x>y)?x:y; }
#endif
template <class T> inline void swap(T& x, T& y) { T t=x; x=y; y=t; }
template <class S, class T> inline void clone(T*& dst, S* src, int n)
{
	dst = new T[n];
	memcpy((void *)dst,(void *)src,sizeof(T)*n);
}
#define INF HUGE_VAL
#define Malloc(type,n) (type *)malloc((n)*sizeof(type))


//
// Kernel Cache
//
// l is the number of total data items
// size is the cache size limit in bytes
//
class Cache
{
public:
	Cache(int l,int size);
	~Cache();

	// request data [0,len)
	// return some position p where [p,len) need to be filled
	// (p >= len if nothing needs to be filled)
	int get_data(const int index, Qfloat **data, int len);
	void swap_index(int i, int j);	// future_option
private:
	int l;
	int size;
	struct head_t
	{
		head_t *prev, *next;	// a cicular list
		Qfloat *data;
		int len;		// data[0,len) is cached in this entry
	};

	head_t* head;
	head_t lru_head;
	void lru_delete(head_t *h);
	void lru_insert(head_t *h);
};

Cache::Cache(int l_,int size_):l(l_),size(size_)
{
	head = (head_t *)calloc(l,sizeof(head_t));	// initialized to 0
	size /= sizeof(Qfloat);
	size -= l * sizeof(head_t) / sizeof(Qfloat);
	lru_head.next = lru_head.prev = &lru_head;
}

Cache::~Cache()
{
	for(head_t *h = lru_head.next; h != &lru_head; h=h->next)
		free(h->data);
	free(head);
}

void Cache::lru_delete(head_t *h)
{
	// delete from current location
	h->prev->next = h->next;
	h->next->prev = h->prev;
}

void Cache::lru_insert(head_t *h)
{
	// insert to last position
	h->next = &lru_head;
	h->prev = lru_head.prev;
	h->prev->next = h;
	h->next->prev = h;
}

int Cache::get_data(const int index, Qfloat **data, int len)
{
	head_t *h = &head[index];
	if(h->len) lru_delete(h);
	int more = len - h->len;

	if(more > 0)
	{
		// free old space
		while(size < more)
		{
			head_t *old = lru_head.next;
			lru_delete(old);
			free(old->data);
			size += old->len;
			old->data = 0;
			old->len = 0;
		}

		// allocate new space
		h->data = (Qfloat *)realloc(h->data,sizeof(Qfloat)*len);
		size -= more;
		swap(h->len,len);
	}

	lru_insert(h);
	*data = h->data;
	return len;
}

void Cache::swap_index(int i, int j)
{
	if(i==j) return;

	if(head[i].len) lru_delete(&head[i]);
	if(head[j].len) lru_delete(&head[j]);
	swap(head[i].data,head[j].data);
	swap(head[i].len,head[j].len);
	if(head[i].len) lru_insert(&head[i]);
	if(head[j].len) lru_insert(&head[j]);

	if(i>j) swap(i,j);
	for(head_t *h = lru_head.next; h!=&lru_head; h=h->next)
	{
		if(h->len > i)
		{
			if(h->len > j)
				swap(h->data[i],h->data[j]);
			else
			{
				// give up
				lru_delete(h);
				free(h->data);
				size += h->len;
				h->data = 0;
				h->len = 0;
			}
		}
	}
}

//
// Kernel evaluation
//
// the static method k_function is for doing single kernel evaluation
// the constructor of Kernel prepares to calculate the l*l kernel matrix
// the member function get_Q is for getting one column from the Q Matrix
//
class Kernel {
public:
	Kernel(int l, svm_node * const * x, const svm_parameter& param);
	virtual ~Kernel();

	static double k_function(const svm_node *x, const svm_node *y,
				 const svm_parameter& param);
	virtual Qfloat *get_Q(int column, int len) const = 0;
	virtual void swap_index(int i, int j) const	// no so const...
	{
		swap(x[i],x[j]);
		if(x_square) swap(x_square[i],x_square[j]);
	}
protected:

	double (Kernel::*kernel_function)(int i, int j) const;

private:
	const svm_node **x;
	double *x_square;

	// svm_parameter
	const int kernel_type;
	const double degree;
	const double gamma;
	const double coef0;

	static double dot(const svm_node *px, const svm_node *py);
	double kernel_linear(int i, int j) const
	{
		return dot(x[i],x[j]);
	}
	double kernel_poly(int i, int j) const
	{
		return pow(gamma*dot(x[i],x[j])+coef0,degree);
	}
	double kernel_rbf(int i, int j) const
	{
		return exp(-gamma*(x_square[i]+x_square[j]-2*dot(x[i],x[j])));
	}
	double kernel_sigmoid(int i, int j) const
	{
		return tanh(gamma*dot(x[i],x[j])+coef0);
	}
};

Kernel::Kernel(int l, svm_node * const * x_, const svm_parameter& param)
:kernel_type(param.kernel_type), degree(param.degree),
 gamma(param.gamma), coef0(param.coef0)
{
	switch(kernel_type)
	{
		case LINEAR:
			kernel_function = &Kernel::kernel_linear;
			break;
		case POLY:
			kernel_function = &Kernel::kernel_poly;
			break;
		case RBF:
			kernel_function = &Kernel::kernel_rbf;
			break;
		case SIGMOID:
			kernel_function = &Kernel::kernel_sigmoid;
			break;
	}

	clone(x,x_,l);

	if(kernel_type == RBF)
	{
		x_square = new double[l];
		for(int i=0;i<l;i++)
			x_square[i] = dot(x[i],x[i]);
	}
	else
		x_square = 0;
}

Kernel::~Kernel()
{
	delete[] x;
	delete[] x_square;
}

double Kernel::dot(const svm_node *px, const svm_node *py)
{
	double sum = 0;
	while(px->index != -1 && py->index != -1)
	{
		if(px->index == py->index)
		{
			sum += px->value * py->value;
			++px;
			++py;
		}
		else
		{
			if(px->index > py->index)
				++py;
			else
				++px;
		}			
	}
	return sum;
}

double Kernel::k_function(const svm_node *x, const svm_node *y,
			  const svm_parameter& param)
{
	switch(param.kernel_type)
	{
		case LINEAR:
			return dot(x,y);
		case POLY:
			return pow(param.gamma*dot(x,y)+param.coef0,param.degree);
		case RBF:
		{
			double sum = 0;
			while(x->index != -1 && y->index !=-1)
			{
				if(x->index == y->index)
				{
					double d = x->value - y->value;
					sum += d*d;
					++x;
					++y;
				}
				else
				{
					if(x->index > y->index)
					{	
						sum += y->value * y->value;
						++y;
					}
					else
					{
						sum += x->value * x->value;
						++x;
					}
				}
			}

			while(x->index != -1)
			{
				sum += x->value * x->value;
				++x;
			}

			while(y->index != -1)
			{
				sum += y->value * y->value;
				++y;
			}
			
			return exp(-param.gamma*sum);
		}
		case SIGMOID:
			return tanh(param.gamma*dot(x,y)+param.coef0);
		default:
			return 0;	/* Unreachable */
	}
}

class Solver_SPOC {
public:
	Solver_SPOC() {};
	~Solver_SPOC() {};
	void Solve(int l, const Kernel& Q, double *alpha_, short *y_,
	double *C_, double eps, int shrinking, int nr_class);
private:
	int active_size;
	double *G;	// gradient of objective function
	short *y;
	bool *alpha_status;	// free:true, bound:false
	double *alpha;
	const Kernel *Q;
	double eps;
	double *C;
	
	int *active_set;
	int l, nr_class;
	bool unshrinked;
	
	double get_C(int i, int m)
	{
		if (y[i] == m)
			return C[m];
		return 0;
	}
	void update_alpha_status(int i, int m)
	{
		if(alpha[i*nr_class+m] >= get_C(i, m))
			alpha_status[i*nr_class+m] = false;
		else alpha_status[i*nr_class+m] = true;
	}
	void swap_index(int i, int j);
	double select_working_set(int &q);
	void solve_sub_problem(double A, double *B, double C, double *nu);
	void reconstruct_gradient();
	void do_shrinking();
};

void Solver_SPOC::swap_index(int i, int j)
{
	Q->swap_index(i, j);
	swap(y[i], y[j]);
	swap(active_set[i], active_set[j]);

	for (int m=0;m<nr_class;m++)
	{	
		swap(G[i*nr_class+m], G[j*nr_class+m]);
		swap(alpha[i*nr_class+m], alpha[j*nr_class+m]);
		swap(alpha_status[i*nr_class+m], alpha_status[j*nr_class+m]);
	}
}

void Solver_SPOC::reconstruct_gradient()
{
	if (active_size == l) return;
	int i, m;

	for (i=active_size*nr_class;i<l*nr_class;i++)
		G[i] = 1;
	for (i=active_size;i<l;i++)
		G[i*nr_class+y[i]] = 0;
		
	for (i=0;i<active_size;i++)
		for (m=0;m<nr_class;m++)
			if (fabs(alpha[i*nr_class+m]) != 0)
			{
				Qfloat *Q_i = Q->get_Q(i,l);
				double alpha_i_m = alpha[i*nr_class+m];
				for (int j=active_size;j<l;j++)
					G[j*nr_class+m] += alpha_i_m*Q_i[j];
			}
}

void Solver_SPOC::Solve(int l, const Kernel&Q, double *alpha_, short *y_,
	double *C_, double eps, int shrinking, int nr_class)
{
	this->l = l;
	this->nr_class = nr_class;
	this->Q = &Q;
	clone(y,y_,l);
	clone(alpha,alpha_,l*nr_class);
	C = C_;
	this->eps = eps;
	unshrinked = false;

	int i, m, q;
	// initialize alpha_status
	{
		alpha_status = new bool[l*nr_class];
		for(i=0;i<l;i++)
			for (m=0;m<nr_class;m++)
				update_alpha_status(i, m);
	}
	
	// initialize active set (for shrinking)
	{
		active_set = new int[l];
		for(int i=0;i<l;i++)
			active_set[i] = i;
		active_size = l;
	}	

	// initialize gradient
	{
		G = new double[l*nr_class];
		
		for (i=0;i<l*nr_class;i++)
			G[i] = 1;
		for (i=0;i<l;i++)
			G[i*nr_class+y[i]] = 0;
		
		for (i=0;i<l;i++)
			for (m=0;m<nr_class;m++)
				if (fabs(alpha[i*nr_class+m]) != 0)
				{
					Qfloat *Q_i = Q.get_Q(i,l);
					double alpha_i_m = alpha[i*nr_class+m];
					for (int j=0;j<l;j++)
						G[j*nr_class+m] += alpha_i_m*Q_i[j];
				}
	}
	
	// optimization step

	int iter = 0, counter = min(l*2, 2000) + 1;
	double *B = new double[nr_class];
	double *nu = new double[nr_class];
	
	while (1)
	{

		// show progress and do shrinking
		
		if (--counter == 0)
		{
			if (shrinking) 
				do_shrinking();
			//	info(".");
			counter = min(l*2, 2000);
		}
	
		if (select_working_set(q) < eps)
		{
			// reconstruct the whole gradient
			reconstruct_gradient();
			// reset active set size and check
			active_size = l;
			//	info("*");info_flush();
			if (select_working_set(q) < eps)
				break;
			else
				counter = 1;	// do shrinking next iteration
		}
		
		++iter;
		
		const Qfloat *Q_q = Q.get_Q(q, active_size);
		double A = Q_q[q];
		for (m=0;m<nr_class;m++)
			B[m] = G[q*nr_class+m] - A*alpha[q*nr_class+m];
		B[y[q]] += A*C[y[q]];

		if (fabs(A) > 0)
			solve_sub_problem(A, B, C[y[q]], nu);
		else
		{
			i = 0;
			for (m=1;m<nr_class;m++)
				if (B[m] > B[i])
					i = m;
			nu[i] = -C[y[q]];
		}
		nu[y[q]] += C[y[q]];

		for (m=0;m<nr_class;m++)
		{
			double d = nu[m] - alpha[q*nr_class+m];
			if (fabs(d) > 1e-12)
			{
				alpha[q*nr_class+m] = nu[m];
				update_alpha_status(q, m);
				for (i=0;i<active_size;i++)
					G[i*nr_class+m] += d*Q_q[i];
			}
		}

	}
	
	delete[] B;
	delete[] nu;
	
	// calculate objective value
	double obj = 0;
	for (i=0;i<l*nr_class;i++)
		obj += alpha[i]*(G[i] + 1);
	for (i=0;i<l;i++)
		obj -= alpha[i*nr_class+y[i]];
	obj /= 2; 
	
	int nSV = 0, nFREE = 0;
	for (i=0;i<nr_class*l;i++)
	{
		if (alpha_status[i])
			nFREE++;
		if (fabs(alpha[i]) > 0)
			nSV++;
	}
	//info("\noptimization finished, #iter = %d, obj = %lf\n",iter, obj);
	//	info("nSV = %d, nFREE = %d\n",nSV,nFREE);

	// put back the solution
	{
		for(int i=0;i<l;i++)
		{
			double *alpha_i = &alpha[i*nr_class];
			double *alpha__i = &alpha_[active_set[i]*nr_class];
			for (int m=0;m<nr_class;m++)
				alpha__i[m] = alpha_i[m];
		}
	}

	delete[] active_set;
	delete[] alpha_status;
	delete[] G;
	delete[] y;
	delete[] alpha;

}

double Solver_SPOC::select_working_set(int &q)
{
	double vio_q = -INF;
	
	int j = 0;
	for (int i=0;i<active_size;i++)
	{
		double lb = -INF, ub = INF;
		for (int m=0;m<nr_class;m++,j++)
		{
			lb = max(G[j], lb);
			if (alpha_status[j])
				ub = min(G[j], ub);
		}
		if (lb - ub > vio_q)
		{
			q = i;
			vio_q = lb - ub;
		}
	}
	
	return vio_q;
}

void Solver_SPOC::do_shrinking()
{
	int i, m;
	double Gm = select_working_set(i);
	if (Gm < eps)
		return;

	// shrink

	for (i=0;i<active_size;i++)
	{
		bool *alpha_status_i = &alpha_status[i*nr_class];
		double *G_i = &G[i*nr_class];
		double th = G_i[y[i]] - Gm/2;
		for (m=0;m<y[i];m++)
			if (alpha_status_i[m] || G_i[m] >= th)
				goto out;
		for (m++;m<nr_class;m++)
			if (alpha_status_i[m] || G_i[m] >= th)
				goto out;
		
		--active_size;
		swap_index(i, active_size);
		--i;
	out:	;
	}
	
	// unshrink, check all variables again before final iterations
	
	if (unshrinked || Gm > 10*eps)	
		return;
	
	unshrinked = true;
	reconstruct_gradient();
	
	for (i=l-1;i>=active_size;i--)
	{
		double *G_i = &G[i*nr_class];
		double th = G_i[y[i]] - Gm/2; 
		for (m=0;m<y[i];m++)
			if (G_i[m] >= th)
				goto out1;
		for (m++;m<nr_class;m++)
			if (G_i[m] >= th)
				goto out1;
		
		swap_index(i, active_size);
		++active_size;
		++i;
	out1:	;
	}
}

int compar(const void *a, const void *b)
{
	if (*(double *)a > *(double *)b)
		return -1;
	else
		if (*(double *)a < *(double *)b)
			return 1;
	return 0;
}
void Solver_SPOC::solve_sub_problem(double A, double *B, double C, double *nu)
{
	int r;
	double *D;
	
	clone(D, B, nr_class+1);
	qsort(D, nr_class, sizeof(double), compar);
	D[nr_class] = -INF;
	
	double phi = D[0] - A*C;
	for (r=0;phi<(r+1)*D[r+1];r++)
		phi += D[r+1];
	delete[] D;
		
	phi /= (r+1);
	for (r=0;r<nr_class;r++)
		nu[r] = min((double) 0, phi - B[r])/A;
}

#ifdef __cplusplus
extern "C" {
#endif
void solvebqp(struct BQP*);
#ifdef __cplusplus
}
#endif

class Solver_B {
public:
	Solver_B() {};
	virtual ~Solver_B() {};

	struct SolutionInfo {
		double obj;
		double *upper_bound;
	};

	virtual void Solve(int l, const Kernel& Q, double *b_, schar *y_,
	double *alpha_, double Cp, double Cn, double eps, SolutionInfo* si, 
	int shrinking, int qpsize);
protected:
	int active_size;
	double *G;		// gradient of objective function
	enum { LOWER_BOUND, UPPER_BOUND, FREE };
	char *alpha_status;	// LOWER_BOUND, UPPER_BOUND, FREE
	double *alpha;
	const Kernel *Q;
	double eps;

	int *active_set;
	double *G_bar;		// gradient, if we treat free variables as 0
	int l;
	bool unshrinked;	// XXX

	int qpsize;
	int *working_set;

	virtual double get_C(int i)
	{
		return (y[i] > 0)? Cp : Cn;
	}
	void update_alpha_status(int i)
	{
		if(alpha[i] >= get_C(i))
			alpha_status[i] = UPPER_BOUND;
		else if(alpha[i] <= 0)
			alpha_status[i] = LOWER_BOUND;
		else alpha_status[i] = FREE;
	}
	bool is_upper_bound(int i) { return alpha_status[i] == UPPER_BOUND; }
	bool is_lower_bound(int i) { return alpha_status[i] == LOWER_BOUND; }
	bool is_free(int i) { return alpha_status[i] == FREE; }
	virtual void swap_index(int i, int j);
	virtual void reconstruct_gradient();
	virtual void shrink_one(int k);
	virtual void unshrink_one(int k);
	double select_working_set(int &q);
	void do_shrinking();
private:
	double Cp, Cn;
	double *b;
	schar *y;
};

void Solver_B::swap_index(int i, int j)
{
	Q->swap_index(i,j);
	swap(y[i],y[j]);
	swap(G[i],G[j]);
	swap(alpha_status[i],alpha_status[j]);
	swap(alpha[i],alpha[j]);
	swap(b[i],b[j]);
	swap(active_set[i],active_set[j]);
	swap(G_bar[i],G_bar[j]);
}

void Solver_B::reconstruct_gradient()
{
	// reconstruct inactive elements of G from G_bar and free variables

	if(active_size == l) return;

	int i;
	for(i=active_size;i<l;i++)
		G[i] = G_bar[i] + b[i];
	
	for(i=0;i<active_size;i++)
		if(is_free(i))
		{
			const Qfloat *Q_i = Q->get_Q(i,l);
			double alpha_i = alpha[i];
			for(int j=active_size;j<l;j++)
				G[j] += alpha_i * Q_i[j];
		}
}

void Solver_B::Solve(int l, const Kernel& Q, double *b_, schar *y_,
	double *alpha_, double Cp, double Cn, double eps, SolutionInfo* si,
	int shrinking, int qpsize)
{
	this->l = l;
	this->Q = &Q;
	b = b_;
	clone(y, y_, l);
	clone(alpha,alpha_,l);
	this->Cp = Cp;
	this->Cn = Cn;
	this->eps = eps;
	this->qpsize = qpsize;
	unshrinked = false;

	// initialize alpha_status
	{
		alpha_status = new char[l];
		for(int i=0;i<l;i++)
			update_alpha_status(i);
	}

	// initialize active set (for shrinking)
	{
		active_set = new int[l];
		for(int i=0;i<l;i++)
			active_set[i] = i;
		active_size = l;
	}

	BQP qp;
	working_set = new int[qpsize];
	qp.eps = eps/10;
	qp.C = new double[qpsize];
	qp.x = new double[qpsize];
	qp.p = new double[qpsize];
	qp.Q = new double[qpsize*qpsize];

	// initialize gradient
	{
		G = new double[l];
		G_bar = new double[l];
		int i;
		for(i=0;i<l;i++)
		{
			G[i] = b[i];
			G_bar[i] = 0;
		}
		for(i=0;i<l;i++)
			if(!is_lower_bound(i))
			{
				Qfloat *Q_i = Q.get_Q(i,l);
				double C_i = get_C(i);
				double alpha_i = alpha[i];
				int j;
				for(j=0;j<l;j++)
					G[j] += alpha_i*Q_i[j];
				if (shrinking)
					if(is_upper_bound(i))
						for(j=0;j<l;j++)
							G_bar[j] += C_i*Q_i[j];
			}
	}

	// optimization step

	int iter = 0;
	int counter = min(l*2/qpsize,2000/qpsize)+1;

	while(1)
	{
		// show progress and do shrinking

		if(--counter == 0)
		{
			counter = min(l*2/qpsize, 2000/qpsize);
			if(shrinking) do_shrinking();
			//info(".");
		}

		int i,j,q;
		if (select_working_set(q) < eps)
		{
			// reconstruct the whole gradient
			reconstruct_gradient();
			// reset active set size and check
			active_size = l;
			//info("*");info_flush();
			if (select_working_set(q) < eps)
				break;
			else
				counter = 1;	// do shrinking next iteration
		}
		
		++iter;

		// construct subproblem
		Qfloat **QB;
		QB = new Qfloat *[q];
		for (i=0;i<q;i++)
			QB[i] = Q.get_Q(working_set[i], active_size);
		qp.n = q;
		for (i=0;i<qp.n;i++)
			qp.p[i] = G[working_set[i]];
		for (i=0;i<qp.n;i++)
		{
			int Bi = working_set[i];
			qp.x[i] = alpha[Bi];
			qp.C[i] = get_C(Bi);
			qp.Q[i*qp.n+i] = QB[i][Bi];
			qp.p[i] -= qp.Q[i*qp.n+i]*alpha[Bi];
			for (j=i+1;j<qp.n;j++)
			{			
				int Bj = working_set[j];
				qp.Q[i*qp.n+j] = qp.Q[j*qp.n+i] = QB[i][Bj];
				qp.p[i] -= qp.Q[i*qp.n+j]*alpha[Bj];
				qp.p[j] -= qp.Q[j*qp.n+i]*alpha[Bi];
			}
		}

		solvebqp(&qp);

		// update G

		for(i=0;i<q;i++)
		{
			double d = qp.x[i] - alpha[working_set[i]];
			if(fabs(d)>1e-12)
			{
				alpha[working_set[i]] = qp.x[i];
				Qfloat *QB_i = QB[i];
				for(j=0;j<active_size;j++)
					G[j] += d*QB_i[j];
			}
		}

		// update alpha_status and G_bar

		for (i=0;i<q;i++)
		{
			int Bi = working_set[i];
			bool u = is_upper_bound(Bi);
			update_alpha_status(Bi);
			if (!shrinking)
				continue;
			if (u != is_upper_bound(Bi))
			{
				Qfloat *QB_i = Q.get_Q(Bi, l);
				double C_i = qp.C[i];
				if (u)
					for (j=0;j<l;j++)
						G_bar[j] -= C_i*QB_i[j];
				else
					for (j=0;j<l;j++)
						G_bar[j] += C_i*QB_i[j];
			}
		}

		delete[] QB;
	}

	// calculate objective value
	{
		double v = 0;
		int i;
		for(i=0;i<l;i++)
			v += alpha[i] * (G[i] + b[i]);

		si->obj = v/2;
	}

	// juggle everything back
	/*{
		for(int i=0;i<l;i++)
			while(active_set[i] != i)
				swap_index(i,active_set[i]);
				// or Q.swap_index(i,active_set[i]);
	}*/

	si->upper_bound = new double[2];
	si->upper_bound[0] = Cp;
	si->upper_bound[1] = Cn;

	//	info("\noptimization finished, #iter = %d\n",iter);

	// put back the solution
	{
		for(int i=0;i<l;i++)
			alpha_[active_set[i]] = alpha[i];
	}

	delete[] active_set;
	delete[] alpha;
	delete[] alpha_status;
	delete[] G;
	delete[] G_bar;
	delete[] y;

	delete[] working_set;
	delete[] qp.p;
	delete[] qp.C;
	delete[] qp.x;
	delete[] qp.Q;
}

// return maximal violation
double Solver_B::select_working_set(int &q)
{
	int i, j, q_2 = qpsize/2;
	double maxvio = 0, max0;
	double *positive_max;
	int *positive_set;

	positive_max = new double[qpsize];
	positive_set = new int[qpsize];
	q = 0;

	for (i=0;i<q_2;i++)
		positive_max[i] = INF/2;
	for (i=0;i<active_size;i++)
	{	
		if(!is_free(i)) continue;
		double v = fabs(G[i]);
		if(v < positive_max[0])
		{
			for (j=1;j<q_2;j++)
			{
				if (v >= positive_max[j])
					break;
				positive_max[j-1] = positive_max[j];
				positive_set[j-1] = positive_set[j];
			}
			positive_max[j-1] = v;
			positive_set[j-1] = i;
		}
	} 
	for (i=0;i<q_2;i++)
		if (positive_max[i] != INF/2)
			working_set[q++] = positive_set[i];
	max0 = q ? positive_max[0] : 0;
	q_2 = qpsize - q;
				
	for (i=0;i<q_2;i++)
		positive_max[i] = -INF;
	for (i=0;i<active_size;i++)
	{
		double v = fabs(G[i]);
		if (is_free(i) && v <= max0) continue;
		if(is_upper_bound(i))
		{
			if(G[i]<0) continue;
		}
		else if(is_lower_bound(i))
		{
			if(G[i]>0) continue;
		}
		if (v > positive_max[0])
		{
			for (j=1;j<q_2;j++)
			{
				if (v <= positive_max[j])
					break;
				positive_max[j-1] = positive_max[j];
				positive_set[j-1] = positive_set[j];
			}
			positive_max[j-1] = v;
			positive_set[j-1] = i;
		}
	}
	for (i=0;i<q_2;i++)
		if (positive_max[i] != -INF)
		{
			working_set[q++] = positive_set[i];
			maxvio = max(maxvio,positive_max[i]);
		}

	delete[] positive_set;
	delete[] positive_max;
	return maxvio;
}

void Solver_B::shrink_one(int k)
{
	swap_index(k, active_size);
}

void Solver_B::unshrink_one(int k)
{
	swap_index(k, active_size);
}

void Solver_B::do_shrinking()
{
	int k;

	double Gm = select_working_set(k);
	if (Gm < eps)
		return;

	// shrink
	
	for(k=0;k<active_size;k++)
	{
		if (is_lower_bound(k))
		{
			if (G[k] <= Gm)
				continue;
		}
		else
			if (is_upper_bound(k))
			{
				if (G[k] >= -Gm)
					continue;
			}
			else
				continue;

		--active_size;
		shrink_one(k);
		--k;	// look at the newcomer
	}

	// unshrink, check all variables again before final iterations

	if (unshrinked || Gm > eps*10)
		return;
	
	unshrinked = true;
	reconstruct_gradient();

	for(k=l-1;k>=active_size;k--)
	{
		if (is_lower_bound(k))
		{
			if (G[k] > Gm)
				continue;
		}
		else
			if (is_upper_bound(k))
			{
				if (G[k] < -Gm)
					continue;
			}
			else
				continue;

		unshrink_one(k);
		active_size++;
		++k;	// look at the newcomer
	}
}

class Solver_B_linear : public Solver_B
{
public:
	Solver_B_linear() {};
	~Solver_B_linear() {};
	int Solve(int l, svm_node * const * x_, double *b_, schar *y_,
	double *alpha_, double *w, double Cp, double Cn, double eps, SolutionInfo* si, 
	int shrinking, int qpsize);
private:
	double get_C(int i)
	{
		return (y[i] > 0)? Cp : Cn;
	}
	void swap_index(int i, int j);
	void reconstruct_gradient();
	double dot(int i, int j);
	double Cp, Cn;
	double *b;
	schar *y;
	double *w;
	const svm_node **x;
};

double Solver_B_linear::dot(int i, int j)
{
	const svm_node *px = x[i], *py = x[j];	
	double sum = 0;
	while(px->index != -1 && py->index != -1)
	{
		if(px->index == py->index)
		{
			sum += px->value * py->value;
			++px;
			++py;
		}
		else
		{
			if(px->index > py->index)
				++py;
			else
				++px;
		}			
	}
	return sum;
}

void Solver_B_linear::swap_index(int i, int j)
{
	swap(y[i],y[j]);
	swap(G[i],G[j]);
	swap(alpha_status[i],alpha_status[j]);
	swap(alpha[i],alpha[j]);
	swap(b[i],b[j]);
	swap(active_set[i],active_set[j]);
	swap(x[i], x[j]);
}

void Solver_B_linear::reconstruct_gradient()
{
	int i;
	for(i=active_size;i<l;i++)
	{
		double sum = 0;
		for (const svm_node *px = x[i];px->index != -1;px++)
			sum += w[px->index]*px->value;
		sum += w[0];
		G[i] = y[i]*sum + b[i];
	}
}

int Solver_B_linear::Solve(int l, svm_node * const * x_, double *b_, schar *y_,
	double *alpha_, double *w, double Cp, double Cn, double eps, SolutionInfo* si,
	int shrinking, int qpsize)
{
	this->l = l;
	clone(x, x_, l);	
	clone(b, b_, l);
	clone(y, y_, l);
	clone(alpha,alpha_,l);
	this->Cp = Cp;
	this->Cn = Cn;
	this->eps = eps;
	this->qpsize = qpsize;
	this->w = w;
	unshrinked = false;

	// initialize alpha_status
	{
		alpha_status = new char[l];
		for(int i=0;i<l;i++)
			update_alpha_status(i);
	}

	// initialize active set (for shrinking)
	{
		active_set = new int[l];
		for(int i=0;i<l;i++)
			active_set[i] = i;
		active_size = l;
	}

	BQP qp;
	working_set = new int[qpsize];
	qp.eps = eps/100;
	qp.C = new double[qpsize];
	qp.x = new double[qpsize];
	qp.p = new double[qpsize];
	qp.Q = new double[qpsize*qpsize];

	// initialize gradient
	{
		G = new double[l];
		int i;
		bool allzero = true;
		for(i=0;i<l;i++)
		{
			G[i] = b[i];
			if(!is_lower_bound(i))
				allzero = false;
		}
		if (!allzero)
			for(i=0;i<l;i++)
			{
				double sum = 0;
				for (const svm_node *px = x[i];px->index != -1;px++)
					sum += w[px->index]*px->value;
				sum += w[0];
				G[i] += y[i]*sum;
			}			
	}

	// optimization step

	int iter = 0;
	int counter = min(l*2/qpsize,2000/qpsize)+1;

	while(1)
	{
		// show progress and do shrinking

		if(--counter == 0)
		{
			counter = min(l*2/qpsize, 2000/qpsize);
			if(shrinking) do_shrinking();
			//	info(".");
		}

		int i,j,q;
		if (select_working_set(q) < eps)
		{
			// reconstruct the whole gradient
			reconstruct_gradient();
			// reset active set size and check
			active_size = l;
			//	info("*");info_flush();
			if (select_working_set(q) < eps)
				break;
			else
				counter = 1;	// do shrinking next iteration
		}
		
		++iter;

		// construct subproblem
		qp.n = q;
		for (i=0;i<qp.n;i++)
			qp.p[i] = G[working_set[i]];
		for (i=0;i<qp.n;i++)
		{
			int Bi = working_set[i];
			qp.x[i] = alpha[Bi];
			qp.C[i] = get_C(Bi);
			qp.Q[i*qp.n+i] = dot(Bi, Bi) + 1;
			qp.p[i] -= qp.Q[i*qp.n+i]*alpha[Bi];
			for (j=i+1;j<qp.n;j++)
			{			
				int Bj = working_set[j];
				qp.Q[i*qp.n+j] = qp.Q[j*qp.n+i] = y[Bi]*y[Bj]*(dot(Bi, Bj) + 1);
				qp.p[i] -= qp.Q[i*qp.n+j]*alpha[Bj];
				qp.p[j] -= qp.Q[j*qp.n+i]*alpha[Bi];
			}
		}

		solvebqp(&qp);

		// update G

		for(i=0;i<q;i++)
		{
			int Bi = working_set[i];
			double d = qp.x[i] - alpha[Bi];
			if(fabs(d)>1e-12)
			{
				alpha[Bi] = qp.x[i];
				update_alpha_status(Bi);
				double yalpha = y[Bi]*d;
				for (const svm_node *px = x[Bi];px->index != -1;px++)
					w[px->index] += yalpha*px->value;
				w[0] += yalpha;
			}
		}
		for(j=0;j<active_size;j++)
		{
			double sum = 0;
			for (const svm_node *px = x[j];px->index != -1;px++)
				sum += w[px->index]*px->value;
			sum += w[0];
			G[j] = y[j]*sum + b[j];
		}

	}

	// calculate objective value
	{
		double v = 0;
		int i;
		for(i=0;i<l;i++)
			v += alpha[i] * (G[i] + b[i]);

		si->obj = v/2;
	}

	// juggle everything back
	/*{
		for(int i=0;i<l;i++)
			while(active_set[i] != i)
				swap_index(i,active_set[i]);
				// or Q.swap_index(i,active_set[i]);
	}*/

	si->upper_bound = new double[2];
	si->upper_bound[0] = Cp;
	si->upper_bound[1] = Cn;

	// info("\noptimization finished, #iter = %d\n",iter);

	// put back the solution
	{
		for(int i=0;i<l;i++)
			alpha_[active_set[i]] = alpha[i];
	}

	delete[] active_set;
	delete[] alpha;
	delete[] alpha_status;
	delete[] G;
	delete[] y;
	delete[] b;

	delete[] working_set;
	delete[] qp.p;
	delete[] qp.C;
	delete[] qp.x;
	delete[] qp.Q;

	return iter;
}


class Solver_MB : public Solver_B
{
public:
	Solver_MB() {};
	~Solver_MB() {};
	void Solve(int l, const Kernel& Q, double lin, double *alpha_,
	short *y_, double *C, double eps, SolutionInfo* si,
	int shrinking, int qpsize, int nr_class, int *count);
private:
	short *y, *yy;
	double *C;
	double lin;
	int *real_i;
	int real_l;

	int nr_class;
	int *start1, *start2;

	double get_C(int i)
	{
		return C[y[i]];
	}
	void swap_index(int i, int j);
	void reconstruct_gradient();
	void shrink_one(int k);
	void unshrink_one(int k);
	void initial_index_table(int *);
	int yyy(int yi, int yyi, int yj, int yyj) const
	{
		int xx = 0;
		if (yi == yj)
			xx++;
		if (yyi == yyj)
			xx++;
		if (yi == yyj)
			xx--;
		if (yj == yyi)
			xx--;
		return xx;
	}
};

void Solver_MB::swap_index(int i, int j)
{
	if (i == j)
		return;
	swap(y[i],y[j]);
	swap(yy[i],yy[j]);
	swap(G[i],G[j]);
	swap(alpha_status[i],alpha_status[j]);
	swap(alpha[i],alpha[j]);
	swap(active_set[i],active_set[j]);
	swap(real_i[i], real_i[j]);
	swap(G_bar[i],G_bar[j]);
}

void Solver_MB::initial_index_table(int *count)
{
	int i, j, k, p, q;

	p = 0;
	for (i=0;i<nr_class;i++)
	{
		q = 0;
		for (j=0;j<nr_class;j++)
		{
			start1[i*nr_class+j] = p;
			start2[i*nr_class+j] = l;
			if (i != j)
				for (k=0;k<count[j];k++)
				{
					yy[p] = i;
					real_i[p] = q;
					active_set[p] = p;
					p++;
					q++;
				}
			else
				q += count[j];
		}
	}
	start1[nr_class*nr_class] = start2[nr_class*nr_class] = l;
}

void Solver_MB::reconstruct_gradient()
{
	// reconstruct inactive elements of G from G_bar and free variables

	if(active_size == l) return;

	int i, j;
	for(i=active_size;i<l;i++)
		G[i] = G_bar[i] + lin;
	
	for(i=0;i<active_size;i++)
		if(is_free(i))
		{
			const Qfloat *Q_i = Q->get_Q(real_i[i],real_l);
			double alpha_i = alpha[i], t;
			int y_i = y[i], yy_i = yy[i], ub, k;
			
			t = 2*alpha_i;
			ub = start2[yy_i*nr_class+y_i+1];
			for (j=start2[yy_i*nr_class+y_i];j<ub;j++)
				G[j] += t*Q_i[real_i[j]];
			ub = start2[y_i*nr_class+yy_i+1];	
			for (j=start2[y_i*nr_class+yy_i];j<ub;j++)
				G[j] -= t*Q_i[real_i[j]];
					
			for (k=0;k<nr_class;k++)
				if (k != y_i && k != yy_i)
				{
					ub = start2[k*nr_class+y_i+1];
					for (j=start2[k*nr_class+y_i];j<ub;j++)
						G[j] += alpha_i*Q_i[real_i[j]];
					ub = start2[yy_i*nr_class+k+1];
					for (j=start2[yy_i*nr_class+k];j<ub;j++)
						G[j] += alpha_i*Q_i[real_i[j]];
							
					ub = start2[y_i*nr_class+k+1];
					for (j=start2[y_i*nr_class+k];j<ub;j++)
						G[j] -= alpha_i*Q_i[real_i[j]];
					ub = start2[k*nr_class+yy_i+1];
					for (j=start2[k*nr_class+yy_i];j<ub;j++)
						G[j] -= alpha_i*Q_i[real_i[j]];	
				}			
		}
}

void Solver_MB::Solve(int l, const Kernel& Q, double lin, double *alpha_,
	short *y_, double *C_, double eps, SolutionInfo* si,
	int shrinking, int qpsize, int nr_class, int *count)
{
	this->l = l;
	this->nr_class = nr_class;
	this->real_l = l/(nr_class - 1);
	this->Q = &Q;
	this->lin = lin;
	clone(y,y_,l);
	clone(alpha,alpha_,l);
	C = C_;
	this->eps = eps;
	this->qpsize = qpsize;
	unshrinked = false;

	// initialize alpha_status
	{
		alpha_status = new char[l];
		for(int i=0;i<l;i++)
			update_alpha_status(i);
	}

	// initialize active set (for shrinking)

	active_set = new int[l];
	active_size = l;
	yy = new short[l];
	real_i = new int[l];
	start1 = new int[nr_class*nr_class+1];
	start2 = new int[nr_class*nr_class+1];

	initial_index_table(count);

	BQP qp;
	working_set = new int[qpsize];
	qp.eps = eps/10;
	qp.C = new double[qpsize];
	qp.x = new double[qpsize];
	qp.p = new double[qpsize];
	qp.Q = new double[qpsize*qpsize];

	// initialize gradient
	{
		G = new double[l];
		G_bar = new double[l];
		int i;
		for(i=0;i<l;i++)
		{
			G[i] = lin;
			G_bar[i] = 0;
		}
		
		for (i=0;i<l;i++)
			if (!is_lower_bound(i))
			{
				Qfloat *Q_i = Q.get_Q(real_i[i], real_l);
				double alpha_i = alpha[i];
				double C_i = get_C(i);
				int y_i = y[i], yy_i = yy[i], ub, j, k;
				
				ub = start1[yy_i*nr_class+y_i+1];
				for (j=start1[yy_i*nr_class+y_i];j<ub;j++)
					G[j] += alpha_i*Q_i[real_i[j]];
				if (shrinking && is_upper_bound(i)) 
					for (j=start1[yy_i*nr_class+y_i];j<ub;j++)
						G_bar[j] += C_i*Q_i[real_i[j]];				
					
				ub = start1[y_i*nr_class+yy_i+1];	
				for (j=start1[y_i*nr_class+yy_i];j<ub;j++)
					G[j] -= alpha_i*Q_i[real_i[j]];
				if (shrinking && is_upper_bound(i)) 
					for (j=start1[y_i*nr_class+yy_i];j<ub;j++)
						G_bar[j] += C_i*Q_i[real_i[j]];					
					
				for (k=0;k<nr_class;k++)
					if (k != y_i && k != yy_i)
					{
						ub = start1[k*nr_class+y_i+1];
						for (j=start1[k*nr_class+y_i];j<ub;j++)
							G[j] += alpha_i*Q_i[real_i[j]];
						if (shrinking && is_upper_bound(i)) 
							for (j=start1[k*nr_class+y_i];j<ub;j++)
								G_bar[j] += C_i*Q_i[real_i[j]];							
						ub = start1[yy_i*nr_class+k+1];
						for (j=start1[yy_i*nr_class+k];j<ub;j++)
							G[j] += alpha_i*Q_i[real_i[j]];
						if (shrinking && is_upper_bound(i)) 
							for (j=start1[yy_i*nr_class+k];j<ub;j++)
								G_bar[j] += C_i*Q_i[real_i[j]];							
							
						ub = start1[y_i*nr_class+k+1];
						for (j=start1[y_i*nr_class+k];j<ub;j++)
							G[j] -= alpha_i*Q_i[real_i[j]];
						if (shrinking && is_upper_bound(i)) 
							for (j=start1[y_i*nr_class+k];j<ub;j++)
								G_bar[j] += C_i*Q_i[real_i[j]];							
						ub = start1[k*nr_class+yy_i+1];
						for (j=start1[k*nr_class+yy_i];j<ub;j++)
							G[j] -= alpha_i*Q_i[real_i[j]];
						if (shrinking && is_upper_bound(i)) 
							for (j=start1[k*nr_class+yy_i];j<ub;j++)
								G_bar[j] += C_i*Q_i[real_i[j]];							
					}
			}
	}

	// optimization step

	int iter = 0;
	int counter = min(l*2/qpsize,2000/qpsize)+1;

	while(1)
	{
		// show progress and do shrinking

		if(--counter == 0)
		{
			counter = min(l*2/qpsize, 2000/qpsize);
			if(shrinking) do_shrinking();
			//	info(".");
		}

		int i,j,q;
		if (select_working_set(q) < eps)
		{
			// reconstruct the whole gradient
			reconstruct_gradient();
			// reset active set size and check
			active_size = l;
			//info("*");info_flush();
			if (select_working_set(q) < eps)
				break;
			else
				counter = 1;	// do shrinking next iteration
			
			short *y0;
			clone(y0,y,l);
			for (i=0;i<l;i++)
				y[active_set[i]] = y0[i];
			delete[] y0;

			char *alpha_status0;
			clone(alpha_status0,alpha_status,l);
			for (i=0;i<l;i++)
				alpha_status[active_set[i]] = alpha_status0[i];
			delete[] alpha_status0;

			double *alpha0;
			clone(alpha0,alpha,l);
			for (i=0;i<l;i++)
				alpha[active_set[i]] = alpha0[i];
			delete[] alpha0;

			double *G0;
			clone(G0,G,l);
			for (i=0;i<l;i++)
				G[active_set[i]] = G0[i];
			delete[] G0;

			double *G_bar0;
			clone(G_bar0,G_bar,l);
			for (i=0;i<l;i++)
				G_bar[active_set[i]] = G_bar0[i];
			delete[] G_bar0;

			initial_index_table(count);
		}

		++iter;	

		// construct subproblem
		Qfloat **QB;
		QB = new Qfloat *[q];
		for (i=0;i<q;i++)
			QB[i] = Q.get_Q(real_i[working_set[i]], real_l);
		qp.n = q;
		for (i=0;i<qp.n;i++)
			qp.p[i] = G[working_set[i]];
		for (i=0;i<qp.n;i++)
		{
			int Bi = working_set[i], y_Bi = y[Bi], yy_Bi = yy[Bi];
			qp.x[i] = alpha[Bi];
			qp.C[i] = get_C(Bi);
			qp.Q[i*qp.n+i] = yyy(y_Bi, yy_Bi, y_Bi, yy_Bi)*
			QB[i][real_i[Bi]];
			qp.p[i] -= qp.Q[i*qp.n+i]*alpha[Bi];
			for (j=i+1;j<qp.n;j++)
			{			
				int Bj = working_set[j];
				qp.Q[i*qp.n+j] = qp.Q[j*qp.n+i] = 
				yyy(y_Bi, yy_Bi, y[Bj], yy[Bj])*QB[i][real_i[Bj]];
				qp.p[i] -= qp.Q[i*qp.n+j]*alpha[Bj];
				qp.p[j] -= qp.Q[j*qp.n+i]*alpha[Bi];
			}
		}

		solvebqp(&qp);

		// update G

		for(i=0;i<q;i++)
		{
			int Bi = working_set[i];
			double d = qp.x[i] - alpha[working_set[i]];
			if(fabs(d) > 1e-12)
			{
				alpha[Bi] = qp.x[i];
				Qfloat *QB_i = QB[i];
				int y_Bi = y[Bi], yy_Bi = yy[Bi], ub, k;

				double t = 2*d;
				ub = start1[yy_Bi*nr_class+y_Bi+1];
				for (j=start1[yy_Bi*nr_class+y_Bi];j<ub;j++)
					G[j] += t*QB_i[real_i[j]];
				ub = start1[y_Bi*nr_class+yy_Bi+1];	
				for (j=start1[y_Bi*nr_class+yy_Bi];j<ub;j++)
					G[j] -= t*QB_i[real_i[j]];
					
				for (k=0;k<nr_class;k++)
					if (k != y_Bi && k != yy_Bi)
					{
						ub = start1[k*nr_class+y_Bi+1];
						for (j=start1[k*nr_class+y_Bi];j<ub;j++)
							G[j] += d*QB_i[real_i[j]];
						ub = start1[yy_Bi*nr_class+k+1];
						for (j=start1[yy_Bi*nr_class+k];j<ub;j++)
							G[j] += d*QB_i[real_i[j]];
							
						ub = start1[y_Bi*nr_class+k+1];
						for (j=start1[y_Bi*nr_class+k];j<ub;j++)
							G[j] -= d*QB_i[real_i[j]];
						ub = start1[k*nr_class+yy_Bi+1];
						for (j=start1[k*nr_class+yy_Bi];j<ub;j++)
							G[j] -= d*QB_i[real_i[j]];	
					}
			}
		}

		// update alpha_status and G_bar

		for (i=0;i<q;i++)
		{
			int Bi = working_set[i];
			bool u = is_upper_bound(Bi);
			update_alpha_status(Bi);
			if (!shrinking)
				continue;
			if (u != is_upper_bound(Bi))
			{
				Qfloat *QB_i = QB[i];
				double C_i = qp.C[i], t = 2*C_i;
				int ub, y_Bi = y[Bi], yy_Bi = yy[Bi], k;
				if (u)
				{
					ub = start1[yy_Bi*nr_class+y_Bi+1];
					for (j=start1[yy_Bi*nr_class+y_Bi];j<ub;j++)
						G_bar[j] -= t*QB_i[real_i[j]];
					ub = start1[y_Bi*nr_class+yy_Bi+1];	
					for (j=start1[y_Bi*nr_class+yy_Bi];j<ub;j++)
						G_bar[j] += t*QB_i[real_i[j]];

					ub = start2[yy_Bi*nr_class+y_Bi+1];
					for (j=start2[yy_Bi*nr_class+y_Bi];j<ub;j++)
						G_bar[j] -= t*QB_i[real_i[j]];
					ub = start2[y_Bi*nr_class+yy_Bi+1];	
					for (j=start2[y_Bi*nr_class+yy_Bi];j<ub;j++)
						G_bar[j] += t*QB_i[real_i[j]];
					
					for (k=0;k<nr_class;k++)
						if (k != y_Bi && k != yy_Bi)
						{
							ub = start1[k*nr_class+y_Bi+1];
							for (j=start1[k*nr_class+y_Bi];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
							ub = start1[yy_Bi*nr_class+k+1];
							for (j=start1[yy_Bi*nr_class+k];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
							
							ub = start1[y_Bi*nr_class+k+1];
							for (j=start1[y_Bi*nr_class+k];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
							ub = start1[k*nr_class+yy_Bi+1];
							for (j=start1[k*nr_class+yy_Bi];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];

							ub = start2[k*nr_class+y_Bi+1];
							for (j=start2[k*nr_class+y_Bi];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
							ub = start2[yy_Bi*nr_class+k+1];
							for (j=start2[yy_Bi*nr_class+k];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
							
							ub = start2[y_Bi*nr_class+k+1];
							for (j=start2[y_Bi*nr_class+k];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
							ub = start2[k*nr_class+yy_Bi+1];
							for (j=start2[k*nr_class+yy_Bi];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
						}
				}
				else
				{
					ub = start1[yy_Bi*nr_class+y_Bi+1];
					for (j=start1[yy_Bi*nr_class+y_Bi];j<ub;j++)
						G_bar[j] += t*QB_i[real_i[j]];
					ub = start1[y_Bi*nr_class+yy_Bi+1];	
					for (j=start1[y_Bi*nr_class+yy_Bi];j<ub;j++)
						G_bar[j] -= t*QB_i[real_i[j]];

					ub = start2[yy_Bi*nr_class+y_Bi+1];
					for (j=start2[yy_Bi*nr_class+y_Bi];j<ub;j++)
						G_bar[j] += t*QB_i[real_i[j]];
					ub = start2[y_Bi*nr_class+yy_Bi+1];	
					for (j=start2[y_Bi*nr_class+yy_Bi];j<ub;j++)
						G_bar[j] -= t*QB_i[real_i[j]];
					
					for (k=0;k<nr_class;k++)
						if (k != y_Bi && k != yy_Bi)
						{
							ub = start1[k*nr_class+y_Bi+1];
							for (j=start1[k*nr_class+y_Bi];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
							ub = start1[yy_Bi*nr_class+k+1];
							for (j=start1[yy_Bi*nr_class+k];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
							
							ub = start1[y_Bi*nr_class+k+1];
							for (j=start1[y_Bi*nr_class+k];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
							ub = start1[k*nr_class+yy_Bi+1];
							for (j=start1[k*nr_class+yy_Bi];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];

							ub = start2[k*nr_class+y_Bi+1];
							for (j=start2[k*nr_class+y_Bi];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
							ub = start2[yy_Bi*nr_class+k+1];
							for (j=start2[yy_Bi*nr_class+k];j<ub;j++)
								G_bar[j] += C_i*QB_i[real_i[j]];
							
							ub = start2[y_Bi*nr_class+k+1];
							for (j=start2[y_Bi*nr_class+k];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
							ub = start2[k*nr_class+yy_Bi+1];
							for (j=start2[k*nr_class+yy_Bi];j<ub;j++)
								G_bar[j] -= C_i*QB_i[real_i[j]];
						}				
				}
			}
		}

		delete[] QB;
	}

	// calculate objective value
	{
		double v = 0;
		int i;
		for(i=0;i<l;i++)
			v += alpha[i] * (G[i] + lin);
		si->obj = v/4;
	}

	clone(si->upper_bound,C,nr_class);
	//info("\noptimization finished, #iter = %d\n",iter);

	// put back the solution
	{
		for(int i=0;i<l;i++)
			alpha_[active_set[i]] = alpha[i];
	}

	delete[] start1;
	delete[] start2;
	delete[] y;
	delete[] yy;
	delete[] real_i;
	delete[] active_set;

	delete[] alpha;
	delete[] alpha_status;
	delete[] G;
	delete[] G_bar;

	delete[] working_set;
	delete[] qp.p;
	delete[] qp.C;
	delete[] qp.x;
	delete[] qp.Q;
}

void Solver_MB::shrink_one(int k)
{
	int i, s = yy[k]*nr_class+y[k], t;
	t = nr_class*nr_class;
	for (i=s+1;i<=t;i++)
		start1[i]--;
	for (i=0;i<=s;i++)
		start2[i]--;
	swap_index(k, start1[s+1]);
	for (i=s+1;i<t;i++)
		swap_index(start1[i], start1[i+1]);
	for (i=0;i<s;i++)
		swap_index(start2[i], start2[i+1]);
}

void Solver_MB::unshrink_one(int k)
{
	int i, s = yy[k]*nr_class+y[k], t;
	swap_index(k, start2[s]);
	for (i=s;i>0;i--)
		swap_index(start2[i], start2[i-1]);
	t = s + 1;
	for (i=nr_class*nr_class;i>t;i--)
		swap_index(start1[i], start1[i-1]);
	t = nr_class*nr_class;
	for (i=s+1;i<=t;i++)
		start1[i]++;
	for (i=0;i<=s;i++)
		start2[i]++;
}

//
// Q matrices for various formulations
//
class BSVC_Q: public Kernel
{ 
public:
	BSVC_Q(const svm_problem& prob, const svm_parameter& param, const schar *y_)
	:Kernel(prob.l, prob.x, param)
	{
		clone(y,y_,prob.l);
		cache = new Cache(prob.l,(int)(param.cache_size*(1<<20)));
		ktype = param.kernel_type;
		expr = param.expr;
		rho = param.rho;
	}
	
	Qfloat *get_Q(int i, int len) const
	{
		Qfloat *data;
		int start;
		if(((start = cache->get_data(i,&data,len)) < len) && ktype != 4)
		{
			for(int j=start;j<len;j++)
				data[j] = (Qfloat)y[i]*y[j]*((this->*kernel_function)(i,j) + 1);
		}
		else if(ktype == 4)
		  {
		    SEXP R_fcall, dummy, ans;
		    PROTECT(R_fcall = lang2(expr, R_NilValue));
		    PROTECT(ans= allocSExp(REALSXP));
		    PROTECT(dummy = allocVector(INTSXP, 1));
		    INTEGER(dummy)[0] = i+1;
		    SETCADR(R_fcall,dummy);
		    ans = eval(R_fcall,rho);
		    for(int j=start;j<len;j++)
		      data[j] = (Qfloat)(y[i]*y[j]*REAL(ans)[j]);
		    UNPROTECT(3);
		  }
		return data;
	}

	void swap_index(int i, int j) const
	{
		cache->swap_index(i,j);
		Kernel::swap_index(i,j);
		swap(y[i],y[j]);
	}

	~BSVC_Q()
	{
		delete[] y;
		delete cache;
	}
private:
        SEXP expr;
        SEXP rho;
        int ktype;
	schar *y;
	Cache *cache;
};

class ONE_CLASS_Q: public Kernel
{
public:
	ONE_CLASS_Q(const svm_problem& prob, const svm_parameter& param)
	:Kernel(prob.l, prob.x, param)
	{
		cache = new Cache(prob.l,(int)(param.cache_size*(1<<20)));
	}
	
	Qfloat *get_Q(int i, int len) const
	{
		Qfloat *data;
		int start;
		if((start = cache->get_data(i,&data,len)) < len)
		{
			for(int j=start;j<len;j++)
				data[j] = (Qfloat)(this->*kernel_function)(i,j);
		}
		return data;
	}

	void swap_index(int i, int j) const
	{
		cache->swap_index(i,j);
		Kernel::swap_index(i,j);
	}

	~ONE_CLASS_Q()
	{
		delete cache;
	}
private:
	Cache *cache;
};

class BONE_CLASS_Q: public Kernel
{
public:
	BONE_CLASS_Q(const svm_problem& prob, const svm_parameter& param)
	:Kernel(prob.l, prob.x, param)
	{
		cache = new Cache(prob.l,(int)(param.cache_size*(1<<20)));
	}
	
	Qfloat *get_Q(int i, int len) const
	{
		Qfloat *data;
		int start;
		if((start = cache->get_data(i,&data,len)) < len)
		{
			for(int j=start;j<len;j++)
				data[j] = (Qfloat)(this->*kernel_function)(i,j) + 1;
		}
		return data;
	}

	~BONE_CLASS_Q()
	{
		delete cache;
	}
private:
	Cache *cache;
};

class BSVR_Q: public Kernel
{ 
public:
	BSVR_Q(const svm_problem& prob, const svm_parameter& param)
	:Kernel(prob.l, prob.x, param)
	{
		l = prob.l;
		cache = new Cache(l,(int)(param.cache_size*(1<<20)));
		sign = new schar[2*l];
		index = new int[2*l];
		for(int k=0;k<l;k++)
		{
			sign[k] = 1;
			sign[k+l] = -1;
			index[k] = k;
			index[k+l] = k;
		}
		q = param.qpsize;
		buffer = new Qfloat*[q];
		for (int i=0;i<q;i++)
			buffer[i] = new Qfloat[2*l];	
		next_buffer = 0;
	}

	void swap_index(int i, int j) const
	{
		swap(sign[i],sign[j]);
		swap(index[i],index[j]);
	}
	
	Qfloat *get_Q(int i, int len) const
	{
		Qfloat *data;
		int real_i = index[i];
		if(cache->get_data(real_i,&data,l) < l)
		{
			for(int j=0;j<l;j++)
				data[j] = (Qfloat)(this->*kernel_function)(real_i,j) + 1;
		}

		// reorder and copy
		Qfloat *buf = buffer[next_buffer];
		next_buffer = (next_buffer+1)%q;
		schar si = sign[i];
		for(int j=0;j<len;j++)
			buf[j] = si * sign[j] * data[index[j]];
		return buf;
	}

	~BSVR_Q()
	{
		delete cache;
		delete[] sign;
		delete[] index;
		for (int i=0;i<q;i++)
			delete[] buffer[i];
		delete[] buffer;
	}
private:
	int l, q;
	Cache *cache;
	schar *sign;
	int *index;
	mutable int next_buffer;
	Qfloat** buffer;
};

//
// construct and solve various formulations
//
static void solve_c_svc(
	const svm_problem *prob, const svm_parameter* param,
	double *alpha, Solver_B::SolutionInfo* si, double Cp, double Cn)
{
	int l = prob->l;
	double *minus_ones = new double[l];
	schar *y = new schar[l];

	int i;

	for(i=0;i<l;i++)
	{
		alpha[i] = 0;
		minus_ones[i] = -1;
		if(prob->y[i] > 0) y[i] = +1; else y[i]=-1;
	}
	
	if (param->kernel_type == LINEAR)
	{
		double *w = new double[prob->n+1];
		for (i=0;i<=prob->n;i++)
			w[i] = 0;
		Solver_B_linear s;
		int totaliter = 0;
		double Cpj = param->Cbegin, Cnj = param->Cbegin*Cn/Cp;
		while (Cpj < Cp)
		{
			totaliter += s.Solve(l, prob->x, minus_ones, y, alpha, w, 
			Cpj, Cnj, param->eps, si, param->shrinking, param->qpsize);
			if (Cpj*param->Cstep >= Cp)
			{
				for (i=0;i<=prob->n;i++)
					w[i] = 0;
				for (i=0;i<l;i++)
				{
					if (y[i] == 1 && alpha[i] >= Cpj)
						alpha[i] = Cp;
					else 
						if (y[i] == -1 && alpha[i] >= Cnj)
							alpha[i] = Cn;
						else
							alpha[i] *= Cp/Cpj;
					double yalpha = y[i]*alpha[i];
					for (const svm_node *px = prob->x[i];px->index != -1;px++)
						w[px->index] += yalpha*px->value;
					w[0] += yalpha;
				}
			}
			else
			{
				for (i=0;i<l;i++)
					alpha[i] *= param->Cstep;
				for (i=0;i<=prob->n;i++)
					w[i] *= param->Cstep;
			}
			Cpj *= param->Cstep;
			Cnj *= param->Cstep;
		}
		totaliter += s.Solve(l, prob->x, minus_ones, y, alpha, w, Cp, Cn,
		param->eps, si, param->shrinking, param->qpsize);
		//info("\noptimization finished, #iter = %d\n",totaliter);
	}
	else
	{
		Solver_B s;
		s.Solve(l, BSVC_Q(*prob,*param,y), minus_ones, y, alpha, Cp, Cn, 
		param->eps, si, param->shrinking, param->qpsize);
	}

	double sum_alpha=0;
	for(i=0;i<l;i++)
		sum_alpha += alpha[i];

	//info("nu = %f\n", sum_alpha/(param->C*prob->l));

	for(i=0;i<l;i++)
		alpha[i] *= y[i];

	delete[] minus_ones;
	delete[] y;
}

static void solve_epsilon_svr(
	const svm_problem *prob, const svm_parameter *param,
	double *alpha, Solver_B::SolutionInfo* si)
{
	int l = prob->l;
	double *alpha2 = new double[2*l];
	double *linear_term = new double[2*l];
	schar *y = new schar[2*l];
	int i;

	for(i=0;i<l;i++)
	{
		alpha2[i] = 0;
		linear_term[i] = param->p - prob->y[i];
		y[i] = 1;

		alpha2[i+l] = 0;
		linear_term[i+l] = param->p + prob->y[i];
		y[i+l] = -1;
	}

	if (param->kernel_type == LINEAR)
	{
		double *w = new double[prob->n+1];
		for (i=0;i<=prob->n;i++)
			w[i] = 0;
		struct svm_node **x = new svm_node*[2*l];
		for (i=0;i<l;i++)
			x[i] = x[i+l] = prob->x[i];
		Solver_B_linear s;
		int totaliter = 0;
		double Cj = param->Cbegin;
		while (Cj < param->C)
		{
			totaliter += s.Solve(2*l, x, linear_term, y, alpha2, w, 
			Cj, Cj, param->eps, si, param->shrinking, param->qpsize);
			if (Cj*param->Cstep >= param->C)
			{
				for (i=0;i<=prob->n;i++)
					w[i] = 0;
				for (i=0;i<2*l;i++)
				{
					if (alpha2[i] >= Cj)
						alpha2[i] = param->C;
					else 
						alpha2[i] *= param->C/Cj;
					double yalpha = y[i]*alpha2[i];
					for (const svm_node *px = x[i];px->index != -1;px++)
						w[px->index] += yalpha*px->value;
					w[0] += yalpha;
				}
			}
			else
			{
				for (i=0;i<2*l;i++)
					alpha2[i] *= param->Cstep;
				for (i=0;i<=prob->n;i++)
					w[i] *= param->Cstep;
			}
			Cj *= param->Cstep;
		}
		totaliter += s.Solve(2*l, x, linear_term, y, alpha2, w, param->C,
			param->C, param->eps, si, param->shrinking, param->qpsize);
		//info("\noptimization finished, #iter = %d\n",totaliter);
	}
	else
	{
		Solver_B s;
		s.Solve(2*l, BSVR_Q(*prob,*param), linear_term, y, alpha2, param->C,
			param->C, param->eps, si, param->shrinking, param->qpsize);
	}

	double sum_alpha = 0;
	for(i=0;i<l;i++)
	{
		alpha[i] = alpha2[i] - alpha2[i+l];
		sum_alpha += fabs(alpha[i]);
	}
	//info("nu = %f\n",sum_alpha/(param->C*l));

	delete[] y;
	delete[] alpha2;
	delete[] linear_term;
}

//
// decision_function
//
struct decision_function
{
	double *alpha;
};

decision_function solve_spoc(const svm_problem *prob, 
	const svm_parameter *param, int nr_class, double *weighted_C)
{
	int i, m, l = prob->l;
	double *alpha = new double[l*nr_class];
	short *y = new short[l];
	
	for (i=0;i<l;i++)
	{
		for (m=0;m<nr_class;m++)
			alpha[i*nr_class+m] = 0;
		y[i] = (short) prob->y[i];	
	}
	
	Solver_SPOC s;
	s.Solve(l, ONE_CLASS_Q(*prob, *param), alpha, y, weighted_C,
	param->eps, param->shrinking, nr_class);
	
	delete[] y;
	decision_function f;
	f.alpha = alpha;
	return f;  	
}

decision_function solve_msvm(const svm_problem *prob, 
	const svm_parameter *param, int nr_class, double *weighted_C, int *count)
{
	Solver_B::SolutionInfo si;
	int i, l = prob->l*(nr_class - 1);
	double *alpha = Malloc(double, l);
	short *y = new short[l];
	
	for (i=0;i<l;i++)
		alpha[i] = 0;
	
	int p = 0;
	for (i=0;i<nr_class;i++)
	{
		int q = 0;
		for (int j=0;j<nr_class;j++)
			if (i != j)
				for (int k=0;k<count[j];k++,q++)
					y[p++] = (short) prob->y[q];
			else
				q += count[j];
	}

	Solver_MB s;
	s.Solve(l, BONE_CLASS_Q(*prob,*param), -2, alpha, y, weighted_C,
	2*param->eps, &si, param->shrinking, param->qpsize, nr_class, count);

	//	info("obj = %f, rho = %f\n",si.obj,0.0);

        // output SVs

	int nSV = 0, nBSV = 0;
	p = 0;
	for (i=0;i<nr_class;i++)
	{
		int q = 0;
		for (int j=0;j<nr_class;j++)
			if (i != j)
				for (int k=0;k<count[j];k++,q++)
				{
					if (fabs(alpha[p]) > 0)
					{
						++nSV;
						if (fabs(alpha[p]) >= si.upper_bound[(int) prob->y[q]])
							++nBSV;
					}
					p++;
				}
			else
				q += count[j];
	}
	//info("nSV = %d, nBSV = %d\n",nSV,nBSV);

	delete[] y;
	delete[] si.upper_bound;
	decision_function f;
	f.alpha = alpha;
	return f;
} 


//
// svm_model
//
struct svm_model
{
	svm_parameter param;	// parameter
	int nr_class;		// number of classes, = 2 in regression
	int l;			// total #SV
	svm_node **SV;		// SVs (SV[l])
	double **sv_coef;	// coefficients for SVs in decision functions (sv_coef[n-1][l])

	// for classification only

	int *label;		// label of each class (label[n])
	int *nSV;		// number of SVs for each class (nSV[n])
				// nSV[0] + nSV[1] + ... + nSV[n-1] = l
	// XXX
	int free_sv;		// 1 if svm_model is created by svm_load_model
				// 0 if svm_model is created by svm_train
};

//
// Interface functions
//


const char *svm_check_parameterb(const svm_problem *prob, const svm_parameter *param)
{
	// svm_type

	int svm_type = param->svm_type;
	if(svm_type != C_SVC &&
	   svm_type != EPSILON_SVR &&
	   svm_type != KBB &&
	   svm_type != SPOC)
		return "unknown svm type";
	
	// kernel_type
	
	int kernel_type = param->kernel_type;
	if(kernel_type != LINEAR &&
	   kernel_type != POLY &&
	   kernel_type != RBF &&
	   kernel_type != SIGMOID)
		return "unknown kernel type";

	// cache_size,eps,C,nu,p,shrinking

	if(kernel_type != LINEAR)
		if(param->cache_size <= 0)
			return "cache_size <= 0";

	if(param->eps <= 0)
		return "eps <= 0";

	if(param->C <= 0)
		return "C <= 0";

	if(svm_type == EPSILON_SVR)
		if(param->p < 0)
			return "p < 0";

	if(param->shrinking != 0 &&
	   param->shrinking != 1)
		return "shrinking != 0 and shrinking != 1";

	if(svm_type == C_SVC ||
	   svm_type == KBB ||
	   svm_type == SPOC)
		if(param->qpsize < 2)
			return "qpsize < 2";

	if(kernel_type == LINEAR)
		if (param->Cbegin <= 0)
			return "Cbegin <= 0";

	if(kernel_type == LINEAR)
		if (param->Cstep <= 1)
			return "Cstep <= 1";

	return NULL;
}



extern "C" {

#include <R.h>
#include <Rinternals.h>
#include <stdio.h>  

  struct svm_node ** sparsifyb (double *x, int r, int c)
  {
    struct svm_node** sparse;
    int         i, ii, count;
    
    sparse = (struct svm_node **) malloc (r * sizeof(struct svm_node *));
    for (i = 0; i < r; i++) {
      /* determine nr. of non-zero elements */
      for (count = ii = 0; ii < c; ii++)
	if (x[i * c + ii] != 0) count++;
      
      /* allocate memory for column elements */
      sparse[i] = (struct svm_node *) malloc ((count + 1) * sizeof(struct svm_node));
      
      /* set column elements */
      for (count = ii = 0; ii < c; ii++)
	if (x[i * c + ii] != 0) {
	  sparse[i][count].index = ii;
	  sparse[i][count].value = x[i * c + ii];
	  count++;
	}
      
      /* set termination element */
      sparse[i][count].index = -1;
    }
    
    return sparse;
  }
  
  void lala3(const svm_problem *prob, const svm_parameter* param, 
		  double *alpha,  double *weighted_C, Solver_B::SolutionInfo* sii, int nr_class, int *count)
  {
    int l = prob->l;
    int i;
    double Cp = param->C;
    double Cn = param->C;
     
    switch(param->svm_type)
      {
      case C_SVC:
	{
	  double *alpha = new double[l];
	  double *minus_ones = new double[l];
	schar *y = new schar[l];

	for(i=0;i<l;i++)
	{
		alpha[i] = 0;
		minus_ones[i] = -1;
		if(prob->y[i] > 0) y[i] = +1; else y[i]=-1;
	}
	
	if (param->kernel_type == LINEAR)
	{
		double *w = new double[prob->n+1];
		for (i=0;i<=prob->n;i++)
			w[i] = 0;
		Solver_B_linear s;
		int totaliter = 0;
		double Cpj = param->Cbegin, Cnj = param->Cbegin*Cn/Cp;
		while (Cpj < Cp)
		{
			totaliter += s.Solve(l, prob->x, minus_ones, y, alpha, w, 
			Cpj, Cnj, param->eps, sii, param->shrinking, param->qpsize);
			if (Cpj*param->Cstep >= Cp)
			{
			  for (i=0;i<=prob->n;i++)
					w[i] = 0;
				for (i=0;i<l;i++)
				{
					if (y[i] == 1 && alpha[i] >= Cpj)
						alpha[i] = Cp;
					else 
						if (y[i] == -1 && alpha[i] >= Cnj)
							alpha[i] = Cn;
						else
							alpha[i] *= Cp/Cpj;
					double yalpha = y[i]*alpha[i];
					for (const svm_node *px = prob->x[i];px->index != -1;px++)
						w[px->index] += yalpha*px->value;
					w[0] += yalpha;
				}
			}
			else
			{
				for (i=0;i<l;i++)
					alpha[i] *= param->Cstep;
				for (i=0;i<=prob->n;i++)
					w[i] *= param->Cstep;
			}
			Cpj *= param->Cstep;
			Cnj *= param->Cstep;
		}
		totaliter += s.Solve(l, prob->x, minus_ones, y, alpha, w, Cp, Cn,
				     param->eps, sii, param->shrinking, param->qpsize);
		//info("\noptimization finished, #iter = %d\n",totaliter);


	}
	}
	break;
      case EPSILON_SVR:
	{
	  double *alpha = new double[2*l];
	double *linear_term = new double[2*l];
	schar *y = new schar[2*l];

	for(i=0;i<l;i++)
	{
		alpha[i] = 0;
		linear_term[i] = param->p - prob->y[i];
		y[i] = 1;

		alpha[i+l] = 0;
		linear_term[i+l] = param->p + prob->y[i];
		y[i+l] = -1;
	}

	if (param->kernel_type == LINEAR)
	{
		double *w = new double[prob->n+1];
		for (i=0;i<=prob->n;i++)
			w[i] = 0;
		struct svm_node **x = new svm_node*[2*l];
		for (i=0;i<l;i++)
			x[i] = x[i+l] = prob->x[i];
		Solver_B_linear s;
		int totaliter = 0;
		double Cj = param->Cbegin;
		while (Cj < param->C)
		{
			totaliter += s.Solve(2*l, x, linear_term, y, alpha, w, 
			Cj, Cj, param->eps, sii, param->shrinking, param->qpsize);
			if (Cj*param->Cstep >= param->C)
			{
				for (i=0;i<=prob->n;i++)
					w[i] = 0;
				for (i=0;i<2*l;i++)
				{
					if (alpha[i] >= Cj)
						alpha[i] = param->C;
					else 
						alpha[i] *= param->C/Cj;
					double yalpha = y[i]*alpha[i];
					for (const svm_node *px = x[i];px->index != -1;px++)
						w[px->index] += yalpha*px->value;
					w[0] += yalpha;
				}
			}
			else
			{
				for (i=0;i<2*l;i++)
					alpha[i] *= param->Cstep;
				for (i=0;i<=prob->n;i++)
					w[i] *= param->Cstep;
			}
			Cj *= param->Cstep;
		}
		totaliter += s.Solve(2*l, x, linear_term, y, alpha, w, param->C,
			param->C, param->eps, sii, param->shrinking, param->qpsize);
		//info("\noptimization finished, #iter = %d\n",totaliter);

	}
	}
	break;
      case KBB:
	{
	  Solver_B::SolutionInfo si;
	  int i, ll = l*(nr_class - 1);
	  //  double *alpha = Malloc(double, l);
	  short *y = new short[ll];
	  
	  for (i=0;i<ll;i++)
	    alpha[i] = 0;
	  
	  int p = 0;
	  for (i=0;i<nr_class;i++)
	    {
	      int q = 0;
	      for (int j=0;j<nr_class;j++)
		if (i != j)
		  for (int k=0;k<count[j];k++,q++)
		    y[p++] = (short) prob->y[q];
		else
		  q += count[j];
	    }
	  
	  	  Solver_MB s;
	   s.Solve(ll, BONE_CLASS_Q(*prob,*param), -2, alpha, y, weighted_C,
	  	  2*param->eps, &si, param->shrinking, param->qpsize, nr_class, count);
	  
	   //info("obj = %f, rho = %f\n",si.obj,0.0);
	  
	  // output SVs
	  
	  int nSV = 0, nBSV = 0;
	  p = 0;
	for (i=0;i<nr_class;i++)
	  {
	    int q = 0;
	    for (int j=0;j<nr_class;j++)
	      if (i != j)
		for (int k=0;k<count[j];k++,q++)
		  {
		    if (fabs(alpha[p]) > 0)
		      {
			++nSV;
			if (fabs(alpha[p]) >= si.upper_bound[(int) prob->y[q]])
			  ++nBSV;
		      }
		    p++;
		  }
			else
			  q += count[j];
	  }
	//info("nSV = %d, nBSV = %d\n",nSV,nBSV);

	delete[] y;
	delete[] si.upper_bound;
	}
	  break;
      case SPOC:
	{
	  int m;
	  short *y = new short[l];
	  for (i=0;i<l;i++)
	    {
	      for (m=0;m<nr_class;m++)
	  	alpha[i*nr_class+m] = 0;
	      y[i] = (short) prob->y[i];	
	    }

	  Solver_SPOC s;
	  s.Solve(l, ONE_CLASS_Q(*prob, *param), alpha, y, weighted_C,
		  param->eps, param->shrinking, nr_class);
	  free(weighted_C);
	  delete[] y;
	}
	break;
      }
  }	

  void lala2(svm_problem *prob, svm_parameter* param,  double *alpha,
	     double *weighted_C, Solver_B::SolutionInfo* si, int nr_class)
  {
    int l = prob->l;
    int i;
   
    	  int m;
	  //	  	  double *alpha = new double[l*nr_class];
	  short *y = new short[l];
	   for (i=0;i<l;i++)
	   {
	     for (m=0;m<nr_class;m++)
	  	alpha[i*nr_class+m] = 0;
	     y[i] = (short) prob->y[i];	
	   }
	  
	   Solver_SPOC s;
	   s.Solve(l, ONE_CLASS_Q(*prob, *param), alpha, y, weighted_C,
		   param->eps, param->shrinking, nr_class);
	   free(weighted_C);
	delete[] y;
  }

  SEXP tron_optim(SEXP x,
		  SEXP r, 
		  SEXP c, 
		  SEXP y, 
		  SEXP nclass,
		  SEXP countc,
		  SEXP kernel_type, 
		  SEXP svm_type, 
		  SEXP cost, 
		  SEXP eps, 
		  SEXP gamma, 
		  SEXP degree, 
		  SEXP coef0,
		  SEXP Cbegin,
		  SEXP Cstep,
		  SEXP weightlabels, 
		  SEXP weights, 
		  SEXP nweights,
		  SEXP weightedc,
		  SEXP cache,
		  SEXP epsilon, 
		  SEXP qpsize,
		  SEXP shrinking,
		  SEXP expr,
		  SEXP rho
		 )
  {
    
    struct svm_parameter param;
    struct svm_problem  prob;
    int i ,*count = NULL;
    double *alpha2 = NULL;
    SEXP alpha3 = NULL;
    int nr_class;
    const char* s;
    struct Solver_B::SolutionInfo si;
    param.svm_type    = *INTEGER(svm_type);
    param.kernel_type = *INTEGER(kernel_type); 
    param.degree      = *REAL(degree); 
    param.gamma       = *REAL(gamma);
    param.coef0       = *REAL(coef0);
    param.cache_size  = *REAL(cache);
    param.eps         = *REAL(epsilon);
    param.C           = *REAL(cost);
    param.Cbegin      = *REAL(Cbegin);
    param.Cstep       = *REAL(Cstep);
    param.qpsize      = *INTEGER(qpsize);
    param.expr        = expr;
    param.rho         = rho;
    nr_class          = *INTEGER(nclass);
    param.nr_weight   = *INTEGER(nweights);
    if (param.nr_weight > 0) {
      param.weight      = (double *) malloc (sizeof(double) * param.nr_weight);
      memcpy (param.weight, REAL(weights), param.nr_weight * sizeof(double));
      param.weight_label = (int *) malloc (sizeof(int) * param.nr_weight);
      memcpy (param.weight_label, INTEGER(weightlabels), param.nr_weight * sizeof(int));
    }
    param.p           = *REAL(eps);
    param.shrinking   = *INTEGER(shrinking);

    /* set problem */
    prob.l = *INTEGER(r);
    prob.y =  (double *) malloc (sizeof(double) * prob.l);
    memcpy(prob.y, REAL(y), prob.l*sizeof(double));
    prob.x = sparsifyb(REAL(x), *INTEGER(r), *INTEGER(c)); 
    s = svm_check_parameterb(&prob, &param);
   
    if (s) 
      printf("%s",s);
    else {
      double *weighted_C = Malloc(double, nr_class);
      memcpy(weighted_C, REAL(weightedc), nr_class*sizeof(double));
      
      if(param.svm_type == 2)
	{ 
	 alpha2 = (double *) malloc (sizeof(double) * prob.l*nr_class);
   	}
      //if(param.svm_type == 1)
      else 
	{ 
	  count = Malloc(int, nr_class);
	  memcpy(count, REAL(countc), nr_class*sizeof(int));
	  alpha2 = (double *) malloc (sizeof(double) * prob.l*(nr_class-1));
	}
      //           svm_model *l=svm_train(&prob,&param);
	          lala3(&prob, &param, alpha2, weighted_C , &si, nr_class, count); 
      //      lala2(&prob, &param, alpha2, weighted_C , &si, nr_class); 
    }
    
    /* clean up memory */
    if (param.nr_weight > 0) {
      free(param.weight);
      free(param.weight_label);
    }
   
     if(param.svm_type == 2)
      {  
	PROTECT(alpha3 = allocVector(REALSXP, (nr_class*prob.l)));
	UNPROTECT(1);  
	for (i = 0; i < prob.l; i++) 
	  free (prob.x[i]); 
	for (i = 0; i <nr_class*prob.l; i++) 
	  REAL(alpha3)[i] = *(alpha2+i); 
      }
     
     if(param.svm_type ==1)
       { 
	 PROTECT(alpha3 = allocVector(REALSXP, ((nr_class-1)*prob.l)));
	 UNPROTECT(1);   
	 free(count);
	 for (i = 0; i < prob.l; i++) 
	   free (prob.x[i]); 
	 for (i = 0; i <(nr_class-1)*prob.l; i++) 
	   REAL(alpha3)[i] = *(alpha2+i); 
       }
     if(param.svm_type == 0||param.svm_type== 3)
      { 
	PROTECT(alpha3 = allocVector(REALSXP, prob.l+1));
	UNPROTECT(1);  
	for (i = 0; i < prob.l; i++) {free (prob.x[i]); REAL(alpha3)[i] = *(alpha2+i); } 
      }
    free(prob.x);
    free(prob.y);
    free(alpha2);
    return alpha3;
  }
 }