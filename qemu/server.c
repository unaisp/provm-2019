#include  <stdio.h>
#include  <stdlib.h>
#include  <string.h>
#include  <sys/types.h>
#include  <sys/ipc.h>
#include  <sys/shm.h>
#include  <signal.h>
#include  <pthread.h>
#include  <math.h>
#include <stdbool.h>
#include  "shm-02.h"
       #include <unistd.h>

#define VM_NO 10
#define SIZE 209715200
#define size1 1000000
key_t ShmKEY;
int ShmID;int temp[SIZE];
struct Memory *ShmPTR;
struct vminfo *vm_info[VM_NO];
int vm_count = 0;
int vm_list[VM_NO];
int sector_list[SIZE];//index indicates sector no and value indicates vm id
/*struct vminfo{
  int id;
  int size;
  };
 */    
void inthandler(int sig){
	signal(sig, SIG_IGN);
	shmdt((void *) ShmPTR);
	printf("Server has detached its shared memory...\n");
	shmctl(ShmID, IPC_RMID, NULL);
	printf("Server has removed its shared memory...\n");
	printf("Server exits...\n");
	exit(0);
}
/*
   void get_vminfo(){
   int i = 0;
   for(i=0;i<=vm_count;i++){
   printf("vm-id=%d,vm-size=%d\n",vm_info[i]->id,vm_info[i]->size);
   }

   }
 */
void alloc_blocks_registered(int size,int id){
	int allocated=0;     
	printf("Received size is %d \n",size);
	int i,j;
	for(i=0,j=0;i<SIZE && allocated <= size-1 ;i++){
		if(sector_list[i]==ShmPTR->id){//sector found 
			sector_list[i]= id;//allocate to a vm
			ShmPTR->ptr[j] = i;
			j++;  
			allocated++;
		}
	}
	printf("last sector allocated was i= %d",i); 
	/*      for(i=0;i<=size;i++){
		printf("temp[%d] is %d\t",i,temp[i]);
		}
	 */      
	//      printf("Shared array is as follows..\n");	

	/*      for(i=0;i<=size;i++){
		printf("ptr[%d] is %d\t",i,ShmPTR->ptr[i]);

		}
	 */	printf("allocated =  %d\n",allocated);

}
void alloc_blocks_new(int size,int id){
	int allocated=0;     
	printf("Received size is %d \n",size);
	int i,j;
	for(i=0,j=0;i<SIZE && allocated <= size-1 ;i++){
		if(sector_list[i]==0){//sector is free
			sector_list[i]= id;//allocate to a vm
			ShmPTR->ptr[j] = i;
			j++;  
			allocated++;
		}
	}
	printf("last sector allocated was i= %d",i); 
	/*      for(i=0;i<=size;i++){
		printf("temp[%d] is %d\t",i,temp[i]);
		}
	 */      
	//      printf("Shared array is as follows..\n");	

	/*      for(i=0;i<=size;i++){
		printf("ptr[%d] is %d\t",i,ShmPTR->ptr[i]);

		}
	 */	printf("allocated =  %d\n",allocated);

}
void  main(int  argc, char *argv[])
{
	double round;
	int allocated=0,count,i,x,current=0;
	bool flag=false;	
	signal(SIGINT,inthandler);//signal handler for ctrt+c
	memset(sector_list,0,209715200*sizeof(sector_list[0]));
	ShmKEY = ftok("/tmp", 'x');     
	ShmID = shmget(ShmKEY, sizeof(struct Memory), IPC_CREAT | 0666);
	printf("key is %x",ShmID);   
	if (ShmID < 0) {
		printf("*** shmget error (server) ***\n");
		exit(1);
	}
	//     printf("Server has received a shared memory of four integers...\n");

	ShmPTR = (struct Memory *) shmat(ShmID, NULL, 0);

	if (!ShmPTR) {
		printf("*** shmat error (server) ***\n");
		exit(1);
	}
	printf("Server has attached the shared memory...\n");
	printf("Virtual Address of the shared memory is : %p\n", (void *)ShmPTR);
	pthread_mutexattr_setpshared(&(ShmPTR->mutexAttr), PTHREAD_PROCESS_SHARED);
	if (pthread_mutex_init(&(ShmPTR->lock),&(ShmPTR->mutexAttr)) != 0) {
		printf("\n mutex init failed\n");
	}
	ShmPTR->vm_done = 0;    
	ShmPTR->status = TAKEN; 
	while(1){
		usleep(1000);
		allocated = 0;
		while(!ShmPTR->vm_done);
		// vm_info[vm_count] = (struct vminfo* )malloc(sizeof(struct vminfo));
		printf("vm has send a message\n");
		//  vm_info[vm_count] -> id = ShmPTR->id;
		//  vm_info[vm_count] -> size = ShmPTR->size;  
		printf("requested size is .. %d\n",ShmPTR->size);

		if(ShmPTR->msg_type == 1){//allocate memory and return physical sectors

			//check if VM was already registered with us
			for(i=1;i<=current;i++){
				if(vm_list[i]== ShmPTR->id){
					flag =  true;
				}
			}	

			round = (double)ShmPTR->size/size1;
			count = ceil(round); 
			printf("Number of rounds required are %f and count is %d \n",round,count);

			if(flag==true){ 
				//vm already registered
				printf("VM already registered..\n");
				flag = false;		
				while(count--){	
					while(!ShmPTR->status==TAKEN);
					ShmPTR->status = NOT_READY;
					if(count==0){//last round		
						alloc_blocks_registered((ShmPTR->size)-allocated,ShmPTR->id);
						goto X;
					}
					alloc_blocks_registered(size1,ShmPTR->id);
					ShmPTR->status = FILLED;
					allocated += size1;
					printf("Round %d completed...\n",count+1);					
				}
			}
			printf("New Vm registered..\n");
			vm_list[++current] = ShmPTR->id;
			while(count--){	
				printf("Status = %d \n", ShmPTR->status);
				while(ShmPTR->status != TAKEN);
				printf("Here 1\n");
				//ShmPTR->status = NOT_READY;
				if(count==0){//last round		
					alloc_blocks_new((ShmPTR->size)-allocated,ShmPTR->id);
					goto X;
				}
				alloc_blocks_new(size1,ShmPTR->id);
				ShmPTR->status = FILLED;
				allocated += size1;
				printf("Round %d completed...\n",count+1);	
				printf("Status = %d \n", ShmPTR->status);				
			}             
			//process message
		}
		if(ShmPTR->msg_type==2){
			printf("resize request received....for block allocation\n");
			round = (double)ShmPTR->size_alloc/size1;
			count = ceil(round);
			printf("Number of rounds required are %f and count is %d \n",round,count);
			while(count--){
				while(!ShmPTR->status==TAKEN);
				ShmPTR->status = NOT_READY;
				if(count==0){//last round               
					alloc_blocks_new(ShmPTR->size_alloc,ShmPTR->id);
					goto X;
				}
				alloc_blocks_new(ShmPTR->size_alloc,ShmPTR->id);
				ShmPTR->status = FILLED;
				allocated += size1;
				printf("Round %d completed...\n",count+1);                                    
			}

		}
		if(ShmPTR->msg_type==3){
			printf("resize request received....for block deallocation\n");
			//get the sector sector from backend and mark it as free...
			round = (double)ShmPTR->size_dealloc/size1;
			count = ceil(round);
			while(count--){
				while(ShmPTR->status != NOT_READY);
				printf("Round = %d\n",count);
				if(count==0){
					for(i=0;i<ShmPTR->size_dealloc;i++){
						x = ShmPTR->ptr[i];
						sector_list[x] = 0;

					}
					ShmPTR->flag = 1;
					printf("Done with last round...going to X\n");	
					goto X;
				}
				for(i=0;i<ShmPTR->size_dealloc;i++){
					x = ShmPTR->ptr[i];
					sector_list[x] = 0; //mark the sector as free
				}	
				allocated += size1;
				ShmPTR->status = READY;   
			} 
		}  
		/*  if(ShmPTR->msg_type==4){
		    printf("VM with id %d  is shutting down...",ShmPTR->id);
		    if(ShmPTR->p==1){//persist all blocks...by default
		    continue;


		    }
		    else{
		//remove the VM from mapping table and deallocate all its blocks				

		dealloc_all(ShmPTR->id);
		}

		}
		 */	
X:ShmPTR->status = FILLED;
  while(ShmPTR->status!=TAKEN);
		ShmPTR->vm_done = 0;
		ShmPTR->status = TAKEN;   
		vm_count++;
		pthread_mutex_unlock(&(ShmPTR->lock));
		printf("Lock released by manager.\n");			

	}
}
