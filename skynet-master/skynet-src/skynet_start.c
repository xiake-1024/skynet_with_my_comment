#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/*
//工作线程监控表项
struct skynet_monitor {
	int version;
	int check_version;
	uint32_t source;
	uint32_t destination;
};


*/


// 监控,管理skynet_monitor指针数组
struct monitor {				

	int count;			// 工作者线程数 skynet内部实际上是  count + 3 多了3个线程的	

	//skynet_monitor指针数组
	struct skynet_monitor ** m;		// monitor 工作线程监控表				
	
	pthread_cond_t cond;	// 条件变量 			
	pthread_mutex_t mutex;	// 互斥锁		 条件变量和互斥锁实现线程的同步 		
	int sleep;		// 睡眠中的工作者线程数 		
	int quit;		//是否退出
};


// 用于线程参数 工作线程
struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};

static int SIG = 0;


//SIGHUP信号的回调函数
static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;	// 服务数为0



//创建线程
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}


// 全部线程都睡眠的情况下才唤醒一个工作线程(即只要有工作线程处于工作状态，则不需要唤醒)
static void
wakeup(struct monitor *m, int busy) {

	//m->count 创建的工作线程数
	//m->sleep 等待的工作线程数
	//busy为0时，表示只有全部................
	
	if (m->sleep >= m->count - busy) {	// 睡眠的线程
		// signal sleep worker, "spurious wakeup" is harmless

		//m的一个做用，让多个线程共享互斥量,条件变量
		pthread_cond_signal(&m->cond);
	}
}


//socket线程  网络事件线程，调用epoll_wait()
static void *
thread_socket(void *p) {
	struct monitor * m = p;

	//初始化线程,初始化线程全局变量
	//#define THREAD_SOCKET 2
	skynet_initthread(THREAD_SOCKET);
	
	for (;;) {

		//首先调用	socket_server_poll(),该函数先检测命令，然后调用 sp_wait(),在linux下，该函数调用epoll_wait()
		int r = skynet_socket_poll();

		//返回0表示要退出
		if (r==0)
			break;

		//表示消息处理错误，或早没有调用epoll_wait()
		if (r<0) {
			
			//#define CHECK_ABORT if (skynet_context_total()==0) break;	// 服务数为0
			CHECK_ABORT
			continue;
		}
		// 有socket消息返回
		wakeup(m,0);	// 全部线程都睡眠的情况下才唤醒一个工作线程(即只要有工作线程处于工作状态，则不需要唤醒)
	}
	return NULL;
}

//销毁struct monitor
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	//销毁每一项
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}

	//销毁锁
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}


// 用于监控是否有消息没有即时处理
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;

	//工作线程数
	int n = m->count;

	//初试化线程局部变量
	skynet_initthread(THREAD_MONITOR);
	
	for (;;) {

		//#define CHECK_ABORT if (skynet_context_total()==0) break;	// 服务数为0
		
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		
		//睡眠5秒
		for (i=0;i<5;i++) {

			//#define CHECK_ABORT if (skynet_context_total()==0) break;	// 服务数为0
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}


// 用于定时器
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	
	skynet_initthread(THREAD_TIMER);
	
	for (;;) {

		//处理超时事件,只是像特定的skynet_context发送消息,并没有特定的处理
		skynet_updatetime();
		CHECK_ABORT
		//m->count工作线程数
		wakeup(m,m->count-1);	// 只要有一个睡眠线程就唤醒，让工作线程热起来
		usleep(2500);
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread

	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);

	pthread_mutex_unlock(&m->mutex);
	return NULL;
}


// 工作线程
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	
	int id = wp->id;
	int weight = wp->weight;
	
	//得到monitor结构体
	struct monitor *m = wp->m;
	
	//获取对应的skynet_monitor
	struct skynet_monitor *sm = m->m[id];

	//初始化线程,初始化线程全局变量  THREAD_WORKER  代表 0
	skynet_initthread(THREAD_WORKER);
	
	struct message_queue * q = NULL;

	//当没有退出的时候
	while (!m->quit) {

		//处理一条消息，调用了skynet_context的回调函数
		q = skynet_context_message_dispatch(sm, q, weight);
		
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {

				// 假装的醒来时无害的 因为 skynet_ctx_msg_dispatch() 可以在任何时候被调用

				//sleep,用来判断要不要调用pthread_cond_signal的
				//等待的工作线程可在socket线程和timer线程中唤醒,
				//前者，有socket消息时会调用一次
				//后者每个刷新时间会唤醒一次

				//将睡眠的工作线程数+1
				++ m->sleep;
				
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
	
				//等待唤醒，即有消息到来
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);


				//睡眠的工作线程数-1
				-- m->sleep;

				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}


//创建线程
static void
start(int thread) {

	// 线程数+3 3个线程分别用于 _monitor _timer  _socket 监控 定时器 socket IO
	pthread_t pid[thread+3]; 

	//创建监控线程的结构体
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	//为 struct skynet_monitor *指针数组分配内存空间
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	
	for (i=0;i<thread;i++) {
		//创建struct skynet_monitor,返回指针,数据都初始化为 0
		m->m[i] = skynet_monitor_new();
	}

	//初始化m的锁
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	//初始化m的条件变量
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}


	//创建监控线程，底层调用的是pthread_create(),thread_monitor是回调函数,m是回调函数的参数
	create_thread(&pid[0], thread_monitor, m);


	//创建timer()定时器线程
	create_thread(&pid[1], thread_timer, m);

	//创建socket网络线程，
	create_thread(&pid[2], thread_socket, m);



	//大概就是，把工作线程分组，前四组每组8个，超过的归入第五组。�
	//A,E组每次调度处理一条消息，B组每次处理n/2条，C组每次处理n/4条，
	//D组每次处理n/8条。是为了均匀使用多核

	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };

/*

struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};
*/
	//用于传递给工作线程的回调函数
	struct worker_parm wp[thread];

	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		//如果 i 的值小于数组长度
		if (i < sizeof(weight)/sizeof(weight[0]))
		{
			wp[i].weight= weight[i];
		}
		else 
		{
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	for (i=0;i<thread+3;i++) {
		// 等待所有线程退出
		pthread_join(pid[i], NULL); 
	}

	//销毁struct monitor
	free_monitor(m);
}


//加载引导模块。主要在Skynet配置文件中定义，默认为boostrap="snlua boostrap",表示引导
//程序加载snlua.so模块，并有snlua服务启动boostrap.lua脚本。如果不使用snlua也可以直接启动
//其他服务的动态库

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	//cmdline 为 snlua boostrap
	
	int sz = strlen(cmdline);
	char name[sz+1];	// snlua
	char args[sz+1];	// boostrap
	sscanf(cmdline, "%s %s", name, args);

	//加载模块 										snlua  boostrap
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		//处理ctx的循环消息队列中的所有消息,调用 回调函数
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

// skynet 启动的时候 初始化
//在skynet_main.c的mian函数中调用

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	struct sigaction sa;

	//信号的回调函数
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);


	//config为保存配置参数变量的结构体
	
	if (config->daemon) {

		//初始化守护线进程，由配置文件确定是否启用。该函数在skynet_damenon.c中
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	//初始化节点模块，用于集群，转发远程节点的消息，该函数定义在skynet_horbor.c中
	skynet_harbor_init(config->harbor);

	//初始化句柄模块，用于给每个Skynet

	//初始化handler_storage,一个存储skynet_context指针的数组
	skynet_handle_init(config->harbor);

	//初始化全局的消息队列模块，这是Skynet的主要数据结构。这个函数定义在skynet_mq.c中
	skynet_mq_init();

	//初始化服务动态库加载模块，主要用于加载符合Skynet服务模块接口的动态链接库。
	//这个函数定义在skynet_module.c中
	//初始化全局的modules,实质就是有一个指针数组，缓存skynet_mudules
	skynet_module_init(config->module_path);

	//初始化定时器模块,该函数定义在skynet_socket.c中
	//初始化 static struct timer * TI 
	skynet_timer_init();

	//初始化网络模块。这个函数定义在skynet_socket.c 中
	//底层初始化了一个 socket_server结构体, 调用的epoll_create()函数
	skynet_socket_init();

	
	skynet_profile_enable(config->profile);

	//创建 skynet_context对象 ,加载日志模块            ("logger",NULL)
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	//加载引导模块。主要在Skynet配置文件中定义，默认为boostrap="snlua boostrap",表示引导
	//程序加载snlua.so模块，并有snlua服务启动boostrap.lua脚本。如果不使用snlua也可以直接启动
	//其他服务的动态库
	bootstrap(ctx, config->bootstrap);

	//创建monitor()监视线程，用create_thread()创建，create_thread()封装了系统函数pthread_create()
//创建socket网络线程，
//创建timer()定时器线程
//创建worker()工作线程，工作线程的数量有Skynet配置文件中的thread=8定义。一般根据
//服务器的CPU核数来

	//config中保存了从配置文件读取的work线程数
	start(config->thread);


	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();


	//释放网络模块
	skynet_socket_free();

	
	if (config->daemon) {
		//退出守护进程。
		daemon_exit(config->daemon);
	}
}
