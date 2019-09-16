#include "Machine.h"
#include "VirtualMachine.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <cstring>

using namespace std;

class Thread
{
	public:
	TVMThreadID ThreadID;
	SMachineContext *MCNTX;
	uint priority;
	uint TVMThreadState, *TVMThreadStateRef;  
	uint memsize;
	void* stack;
	uint context;
	void* param;
	TVMStatus threadstate;
	TVMThreadEntry entry;
	uint sleep;
	uint isblocking;
	uint isPatient;
	void* calldata;	
	uint MutexID;
	int result;
};

class Mutex
{
	public:
	uint MutexID;
	uint OwnerID; // owner threadID
	uint lock;
	vector<Thread*> mutexAcquire;
};

class MemBlock
{
	public:
	void* blockBase;
	uint blockSize;
	char inUse;
};

class MemPool
{
	public:
	TVMMemoryPoolID poolID;
	void* poolBase;
	uint poolSize;
	vector<MemBlock*> memBlocks;
};

volatile uint Sleep;
volatile uint count = 0;
volatile uint numThreads = 0;
volatile uint numMutex = 0;
volatile uint totaltick = 0;
void* baseAddress = NULL;
uint global_tickms;
Thread* temp;
Thread *runningthread;
vector<Thread*> ThreadControlBlock;
vector<Thread*> readyQ, waitQ; // High priority to low priority
vector<Mutex*> MXT;
TVMMutexID MutexReadWrite;
const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;
vector<MemPool*> memPools;
volatile uint numMemPools = 0;

void insertRQ(Thread* thread )
{
	if(readyQ.size() == 0 || thread->priority == 0) // if RQ is empty or idle is being entered
	{
		readyQ.push_back(thread);
		return;
	}
	vector<Thread*>::iterator i;
	for(i = readyQ.begin();  i != readyQ.end()&&thread->priority <= (*i)->priority ; i++);
	readyQ.insert(i, thread);
}

void dequeueRQ(TVMThreadID thread)
{
	vector<Thread*>::iterator i;

	for(i = readyQ.begin(); i != readyQ.end(); i++)
	{
		if((*i)->ThreadID == thread)
		{
			readyQ.erase(i);
			return;
		}
	}
}

void insertWQ(Thread* thread )
{
	if(waitQ.size() == 0)
	{
		waitQ.push_back(thread);
		return;
	}
	else
	{
		vector<Thread*>::iterator i;
		for(i = waitQ.begin();  i != waitQ.end()&&thread->priority <= (*i)->priority ; i++);
		waitQ.insert(i, thread);
	}
}

void dequeueWQ(TVMThreadID thread)
{
	vector<Thread*>::iterator i;

	for(i = waitQ.begin(); i != waitQ.end(); i++)
	{
		if((*i)->ThreadID == thread)
		{
			waitQ.erase(i);	
			return;
		}
	}
}

void insertMQ(Mutex* mxt, Thread* thread )
{
	if(mxt->mutexAcquire.size() == 0)
	{
		mxt->mutexAcquire.push_back(thread);
		return;
	}
	else
	{
		vector<Thread*>::iterator i;
		for(i = mxt->mutexAcquire.begin();  i != mxt->mutexAcquire.end()&&thread->priority <= (*i)->priority ; i++);
		mxt->mutexAcquire.insert(i, thread);
		//mxt->mutexAcquire.push_back(thread);
	}
}

void dequeueMQ(Mutex* mxt, TVMThreadID thread)
{
	vector<Thread*>::iterator i;

	for(i = mxt->mutexAcquire.begin(); i != mxt->mutexAcquire.end(); i++)
	{
		if((*i)->ThreadID == thread)
		{
			mxt->mutexAcquire.erase(i);	
			return;
		}
	}
}

MemPool* getMemPoolByID(TVMMemoryPoolID id)
{	
	vector<MemPool*>::iterator  it;
	
	for (it = memPools.begin(); it != memPools.end(); it++)
	{
		if ((*it)->poolID == id)
		{
			return (*it);
		}
	}
	
	return NULL;
}

extern "C" 
{
	
	TVMMainEntry VMLoadModule(const char *module);
	void VMUnloadModule(void);
}

void sleepdecrement()
{
	for(uint i = 0; i < waitQ.size(); i++)
	{	
		if((waitQ[i]->sleep) > 0 )
		{
			waitQ[i]->sleep--;
		}
		if(waitQ[i]-> sleep == 0 && waitQ[i]->isblocking == 0)
		{
			
			waitQ[i]->threadstate = VM_THREAD_STATE_READY;
			temp = waitQ[i];
			dequeueWQ(waitQ[i]->ThreadID);

			insertRQ(temp);
		}
		else if(waitQ[i]-> sleep == 0 && waitQ[i]->isblocking == 1)
		{
			//dequeueMQ(MXT[waitQ[i]->MutexID], waitQ[i]->ThreadID);
			waitQ[i]->threadstate = VM_THREAD_STATE_READY;
			insertRQ(waitQ[i]);
			dequeueWQ(waitQ[i]->ThreadID);
			//MXT[waitQ[i]->MutexID]->mutexAcquire;
			
			//waitQ[i]->MutexID = -1;
		}
		
	}
}

void Idle(void* )
{
	MachineEnableSignals();
	while(1)
	{
		//count++;
	}
}

void Scheduler()
{
	if (runningthread == NULL) return;
	
	temp = runningthread;
	
	if(runningthread->threadstate == VM_THREAD_STATE_RUNNING || runningthread->threadstate == VM_THREAD_STATE_READY)
	{
		runningthread->threadstate = VM_THREAD_STATE_READY;
		insertRQ(temp);
	}
	if(readyQ.size() == 0) return;
	Thread* toRun = readyQ[0];
	dequeueRQ(readyQ[0]->ThreadID);
	toRun->threadstate = VM_THREAD_STATE_RUNNING;
	runningthread = toRun;
	MachineContextSwitch(temp->MCNTX, toRun->MCNTX);
}

void Quantum(void*)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	sleepdecrement();	
	totaltick++;
	Scheduler();
	MachineResumeSignals(&sigstate);
}

void callback(void* contact, int result)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	dequeueWQ(((Thread*)contact)->ThreadID);
	insertRQ(((Thread*)contact));
	((Thread*)contact)->threadstate = VM_THREAD_STATE_READY;
	((Thread*)contact)->calldata = contact;
	((Thread*)contact)->result = result;
	Scheduler();
	MachineResumeSignals(&sigstate);
}

TVMStatus VMStart(int tickms, TVMMemorySize heapsize, TVMMemorySize sharedsize, int argc, char *argv[])
{
	baseAddress = MachineInitialize(sharedsize);
	
	MemPool* systemMemPool = new MemPool();
	systemMemPool->poolBase = new uint8_t[heapsize];
	systemMemPool->poolID = numMemPools;
	systemMemPool->poolSize = heapsize;
	numMemPools++;
	MemBlock* baseBlock = new MemBlock();
	baseBlock->blockBase = systemMemPool->poolBase;
	baseBlock->blockSize = heapsize;
	baseBlock->inUse = 0;
	systemMemPool->memBlocks.push_back(baseBlock);
	memPools.push_back(systemMemPool);
	
	uint qid;
	
	global_tickms = tickms;
	TVMMainEntry module = VMLoadModule(argv[0]);
	if(module == NULL)
		return VM_STATUS_FAILURE;

	TVMThreadID temp;
	VMThreadCreate(Idle, NULL, 0x100000, 0x00, &temp);
	VMThreadActivate(temp); 
	
	Thread* tcb = new Thread();
	tcb->ThreadID = 1;
	tcb->threadstate = VM_THREAD_STATE_RUNNING;
	tcb->priority = 2;
	tcb->MCNTX = new SMachineContext();
	ThreadControlBlock.push_back(tcb);
	runningthread = tcb;
	
	numThreads++;
	
	VMMutexCreate(&MutexReadWrite);
	
	MachineEnableSignals();
	MachineRequestAlarm(tickms*1000, Quantum, &qid);
	module(argc,argv);
	
	VMUnloadModule();
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadSleep(TVMTick tick)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	if(tick == VM_TIMEOUT_INFINITE)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	runningthread->sleep = tick;
	if(tick == VM_TIMEOUT_IMMEDIATE)
	{
		runningthread->threadstate = VM_THREAD_STATE_READY;
		dequeueRQ(runningthread->ThreadID);
		insertRQ(runningthread);
		Scheduler();	
		MachineResumeSignals(&sigstate);	
		return VM_STATUS_SUCCESS;
	}
	
	runningthread->threadstate = VM_THREAD_STATE_WAITING;
	dequeueRQ(runningthread->ThreadID);
	insertWQ(runningthread);
	
	Scheduler();	
	MachineResumeSignals(&sigstate);	
	return VM_STATUS_SUCCESS;
}

void skeletonentry(void* thread)
{
	runningthread = (Thread*)thread; // sets new running thread
	MachineEnableSignals();
	((Thread*)thread)->entry(((Thread*)thread)->param);
	VMThreadTerminate(runningthread->ThreadID);
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(entry == NULL || tid == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	Thread* tcb = new Thread();
	*tid = numThreads;
	tcb->ThreadID = numThreads;
	VMMemoryPoolAllocate(0, memsize, &(tcb->stack));
	//tcb->stack = malloc(memsize);
	tcb->priority = prio;
	tcb->memsize = memsize;
	tcb->entry = entry;
	tcb->threadstate = VM_THREAD_STATE_DEAD;
	tcb->param = param;
	tcb->MCNTX = new SMachineContext();
	tcb->MutexID = -1; // no mutex;
	ThreadControlBlock.push_back(tcb);
	numThreads++;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadDelete(TVMThreadID thread)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	//thread = find(thread);
	if(thread >= ThreadControlBlock.size() || ThreadControlBlock[thread] == NULL) // no find
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(ThreadControlBlock[thread]->threadstate != VM_THREAD_STATE_DEAD)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_STATE;
	}		
	free(ThreadControlBlock[thread]);
	ThreadControlBlock[thread] = NULL;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadActivate(TVMThreadID thread)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	//thread = find(thread);
	if(thread >= ThreadControlBlock.size() || ThreadControlBlock[thread] == NULL) // no find
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(ThreadControlBlock[thread]->threadstate != VM_THREAD_STATE_DEAD)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	MachineContextCreate( ThreadControlBlock[thread]->MCNTX , skeletonentry, ThreadControlBlock[thread], ThreadControlBlock[thread]->stack, ThreadControlBlock[thread]->memsize );
	ThreadControlBlock[thread]->threadstate = VM_THREAD_STATE_READY;
	insertRQ(ThreadControlBlock[thread]);
	if(runningthread!=NULL)
		if(ThreadControlBlock[thread]->priority > runningthread->priority)
			Scheduler();
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID thread)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	//thread = find(thread);
	if(thread >= ThreadControlBlock.size() || ThreadControlBlock[thread] == NULL) // no find
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	
	if(ThreadControlBlock[thread]->threadstate == VM_THREAD_STATE_DEAD)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	
	if(ThreadControlBlock[thread]->threadstate == VM_THREAD_STATE_READY)
		dequeueRQ(thread);
	
	uint mxtid = ThreadControlBlock[thread]->MutexID;
	if(mxtid != (uint)-1)
	if(MXT[mxtid]->OwnerID == thread)
	{
		MXT[mxtid]->lock = 0;
		MXT[mxtid]->OwnerID = -1;
		if(MXT[mxtid]->mutexAcquire.size()!= 0)
		{
			Thread* t = MXT[mxtid]->mutexAcquire[0];
			t->threadstate = VM_THREAD_STATE_READY;
			t->isblocking = 0;
			t->sleep=0;
			dequeueMQ(MXT[mxtid],t->ThreadID);
			dequeueWQ(t->ThreadID);
			insertRQ(t);
		}
	}
	if(ThreadControlBlock[thread]->threadstate == VM_THREAD_STATE_WAITING)
	{
		dequeueWQ(thread);
		if(ThreadControlBlock[thread]->isblocking)
		{
			dequeueMQ(MXT[mxtid],ThreadControlBlock[thread]->ThreadID);
		}
	}
	
	ThreadControlBlock[thread]->threadstate = VM_THREAD_STATE_DEAD;
	Scheduler();
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadID(TVMThreadIDRef threadref)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(threadref == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	*threadref = runningthread->ThreadID;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
{	
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	//thread = find(thread);
	if(thread >= ThreadControlBlock.size() || ThreadControlBlock[thread] == NULL) // no find
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(stateref == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	*stateref = ThreadControlBlock[thread]->threadstate;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(mutexref == NULL)
	{	
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	Mutex* mutex = new Mutex();
	*mutexref = numMutex;
	mutex->MutexID = numMutex;
	mutex->lock = 0;
	mutex->OwnerID = -1;
	MXT.push_back(mutex);	
	numMutex++;
	MachineResumeSignals(&sigstate);
	
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutex)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(mutex > numMutex || MXT[mutex] == NULL)
	{	
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(MXT[mutex]->OwnerID != (uint)-1)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	free(MXT[mutex]);
	MXT[mutex] = NULL;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(ownerref == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	if(mutex >= MXT.size())
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	*ownerref = MXT[mutex]->OwnerID;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(mutex >= MXT.size() || MXT[mutex] == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(MXT[mutex]-> lock == 1 && ThreadControlBlock[ MXT[mutex]->OwnerID ]->threadstate == VM_THREAD_STATE_DEAD)		
	{
		MXT[mutex]->lock = 0;
		MXT[mutex]->OwnerID = -1;
	}
	if(timeout == VM_TIMEOUT_IMMEDIATE && MXT[mutex]->lock == 1)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	if(MXT[mutex]->lock == 1)
	{
		runningthread->threadstate= VM_THREAD_STATE_WAITING;
		insertWQ(runningthread);
		runningthread->isblocking = 1;
		if(timeout == VM_TIMEOUT_INFINITE)
			timeout = -1;
		runningthread->sleep = timeout;
		runningthread->MutexID = mutex;
		insertMQ(MXT[mutex], runningthread);
		Scheduler();
		if(MXT[runningthread->MutexID]->lock == 1)
		{
			dequeueMQ(MXT[runningthread->MutexID], runningthread->ThreadID);
			MachineResumeSignals(&sigstate);
			
			return VM_STATUS_FAILURE;
		}
	}
	MXT[mutex]->lock = 1;
	MXT[mutex]->OwnerID = runningthread->ThreadID;
	runningthread->MutexID = mutex;
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
} 

TVMStatus VMMutexRelease(TVMMutexID mutex)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(mutex >= MXT.size() || MXT[mutex] == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(MXT[mutex]->OwnerID != runningthread->ThreadID)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	MXT[mutex]->lock = 0;
	MXT[mutex]->OwnerID = -1; // not owned
	if(MXT[mutex]->mutexAcquire.size() > 0)
	{
		Thread* it = MXT[mutex]->mutexAcquire[0];
		it->isblocking = 0;
		it->threadstate = VM_THREAD_STATE_READY;
		dequeueWQ(it->ThreadID);
		insertRQ(it);
		dequeueMQ(MXT[mutex],it->ThreadID);
		MXT[mutex]->lock = 1;
		MXT[mutex]->OwnerID = it->ThreadID; // not owned
	}
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMTickMS(int* tickmsref)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(tickmsref == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	*tickmsref = global_tickms;
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(tickref == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}		
	*tickref = totaltick;
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(data == NULL || length == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	VMMutexAcquire(MutexReadWrite, VM_TIMEOUT_INFINITE);
	
	int len = *length;
	int byteWrite = 0;
	uint8_t* addressWrite = (uint8_t*)data;
	
	while(len > 0)
	{	
		//write packet
		byteWrite = (len > 512 ? 512 : len);
		memcpy(baseAddress, addressWrite, (size_t)byteWrite);
		len -= 512;
		addressWrite += 512;
		MachineFileWrite(filedescriptor, baseAddress, byteWrite, callback, runningthread);
		runningthread->threadstate = VM_THREAD_STATE_WAITING;
		insertWQ(runningthread);
		Scheduler();
	}
	
	VMMutexRelease(MutexReadWrite);
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(data == NULL || length == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	VMMutexAcquire(MutexReadWrite, VM_TIMEOUT_INFINITE);
	
	int len = *length;
	*length = 0;
	int byteRead = 0;
	uint8_t* addressRead = (uint8_t*)data;
	
	while (len > 0)
	{
		byteRead = (len > 512 ? 512 : len);
		MachineFileRead(filedescriptor, baseAddress, byteRead, callback, runningthread);
		len -= 512;
		insertWQ(runningthread);
		runningthread->threadstate = VM_THREAD_STATE_WAITING;
		Scheduler();
		*length += runningthread->result;
		memcpy(addressRead, baseAddress, byteRead);
		addressRead += 512;
	}
	
	VMMutexRelease(MutexReadWrite);
	
	if(runningthread->result < 0)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	if(*filename == NULL || filedescriptor == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	MachineFileOpen(filename,flags,mode, callback, runningthread);
	runningthread->threadstate = VM_THREAD_STATE_WAITING;
	insertWQ(runningthread);
	Scheduler();
	if(runningthread->result < 0)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	*filedescriptor = runningthread->result;
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileClose(int filedescriptor)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	MachineFileClose(filedescriptor, callback, runningthread);
	runningthread->threadstate = VM_THREAD_STATE_WAITING;
	insertWQ(runningthread);
	Scheduler();
	if(runningthread->result < 0)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	MachineFileSeek(filedescriptor, offset, whence, callback, runningthread);
	insertWQ(runningthread);
	runningthread->threadstate = VM_THREAD_STATE_WAITING;
	Scheduler();
	*newoffset = runningthread->result;
	if(runningthread->result <0)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	if (base == NULL || memory == NULL || size == 0)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MemPool* tempPool = new MemPool();
	tempPool->poolBase = base;
	tempPool->poolSize = size;
	*memory = numMemPools;
	tempPool->poolID = numMemPools;
	numMemPools++;
	MemBlock* tempBlock = new MemBlock();
	tempBlock->blockBase = tempPool->poolBase;
	tempBlock->blockSize = tempPool->poolSize;
	tempBlock->inUse = 0;
	tempPool->memBlocks.push_back(tempBlock);
	memPools.push_back(tempPool);
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	if (memory < 0 || memory > numMemPools)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MemPool* pool = getMemPoolByID(memory);
	
	if (pool == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	int flag = 0;
	vector<MemBlock*>::iterator it;
	for (it = pool->memBlocks.begin(); it != pool->memBlocks.end(); it++)
	{
		if ((*it)->inUse == 1)
		{
			flag = 1;
			break;
		}
	}
	if (flag)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	
	vector<MemPool*>::iterator jt;
	for (jt = memPools.begin(); jt != memPools.end(); jt++)
	{
		if ((*jt) == pool)
		{
			break;
		}
	}
	
	free(*jt);
	memPools.erase(jt);
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	MemPool* pool = getMemPoolByID(memory);
	
	if(pool == NULL || bytesleft == NULL)
	{
		MachineResumeSignals(&sigstate);	
		return VM_STATUS_ERROR_INVALID_PARAMETER;	
	}
	
	*bytesleft = 0;

	vector<MemBlock*>::iterator it;
	for(it = pool->memBlocks.begin(); it != pool->memBlocks.end(); it++)
		if((*it)->inUse == 0)
			*bytesleft += (*it)->blockSize;
	
	MachineResumeSignals(&sigstate);	
	return VM_STATUS_SUCCESS;;
}

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	MemPool* pool = getMemPoolByID(memory);
	
	if (pool == NULL || size == 0 || pointer == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	size = (size + 0x3F) & (~0x3F);
	*pointer = NULL;
	
	vector<MemBlock*>::iterator it;
	
	for (it = pool->memBlocks.begin(); it != pool->memBlocks.end(); it++)
	{
		if ((*it)->inUse == 0 && (*it)->blockSize >= size)
		{
			(*it)->inUse = 1;
			*pointer = (*it)->blockBase;
			MemBlock* rightBlock = new MemBlock();
			rightBlock->blockSize = (*it)->blockSize - size;
			rightBlock->blockBase = (uint8_t*)((*it)->blockBase) + size;
			rightBlock->inUse = 0;
			(*it)->blockSize = size;
			if (rightBlock->blockSize > 0)
				pool->memBlocks.insert(it + 1, rightBlock);
			break;
		}
	}
	
	if (*pointer == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
	}
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
{
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	MemPool* pool = getMemPoolByID(memory);
	
	if (pool == NULL || pointer == NULL)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	//deallocate
	int flag = 0;
	vector<MemBlock*>::iterator it;
	for (it = pool->memBlocks.begin(); it != pool->memBlocks.end(); it++)
	{
		if ((*it)->blockBase == pointer)
		{
			flag = 1;
			(*it)->inUse = 0;
			//merge
			if (it != pool->memBlocks.begin())
			{
				if ((*(it - 1))->inUse == 0)
				{
					(*(it - 1))->blockSize += (*it)->blockSize;
					free(*it);
					pool->memBlocks.erase(it);
				}
			}
			if (it != pool->memBlocks.end())
			{
				if (it + 1 != pool->memBlocks.end()) 
				{
					if ((*(it + 1))->inUse == 0)
					{
						(*it)->blockSize += (*(it + 1))->blockSize;
						free(*(it + 1));
						pool->memBlocks.erase(it + 1);
					}
				}
			}
			break;
		}
	}
	if (!flag)
	{
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}