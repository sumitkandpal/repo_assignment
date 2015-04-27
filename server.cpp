#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <unordered_map>
#include <pthread.h>
#include <iterator>
#include <cerrno>
//using namespace std;

#define DEBUG

enum
{
 	BUF_SIZE=1024,
	HEADER_SIZE=24
};
 struct packetParam
 {
	unsigned char  magic;
	unsigned char  opCode;
	unsigned short keyLength;
	unsigned char  extraLength;
	unsigned char  dataType;
	unsigned short status;
	unsigned int totalBodyLength;
	unsigned int opaque;
	unsigned int cas;
	
	
	
 };
 
typedef struct packetParam packetParam;
void depacketize(void *buff,packetParam *param);
void packetize(void *buff,packetParam *param);
void* threadMain (void* targs);


pthread_mutex_t rwmutex =PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t oktoread=PTHREAD_COND_INITIALIZER;
pthread_cond_t oktowrite=PTHREAD_COND_INITIALIZER;

int AW=0;//active writer
int AR=0;//active reader
int WW=0;//waiting writer
int WR=0;//waiting reader

//hash table for key/values
std::unordered_map<std::string,std::string> hash1;


/************************************************************************************
 * main
 *
 * creates socket ,bind and start listening for incoming connection.
 * creates thread for the client and thread function handles the client 
 ***********************************************************************************/
int main()
{

	int socklisten=socket(AF_INET,SOCK_STREAM,0);
	int sockserv;
	
	//local address construction	
	struct sockaddr_in  addr;
	addr.sin_family=AF_INET;
	addr.sin_port=htons(11211);
	addr.sin_addr.s_addr=htonl(INADDR_ANY);

	int a=1;
	
	//to allow binding to an port or address already in use	
	setsockopt(socklisten,SOL_SOCKET,SO_REUSEADDR,&a,sizeof(a));
	
	//bind to the local addr
	if( bind(socklisten,(struct sockaddr*)(&addr),sizeof(addr))!=0)
	{
		std::cout<<"unable to bind\n";
		return 0;
	}
	
	//listen for the incoming conections	
	listen(socklisten,5);

	//waiting for accepting conection from client
	std::cout<<"Server starts Ready to accept connections\n";
	while( (sockserv=accept(socklisten,0,0))!=0)
	{	
		pthread_t tid;
		//client thread create.threadMain function will process the client
		int ret=pthread_create(&tid,NULL,threadMain,(void*)(&sockserv));
		if(ret!=0)
		{
			std::cout<<"thread creation failed"<<strerror(ret);
			return 0;
		}

			
	}
	
	return 0;
}


/************************************************************************************
 * threadMain
 *
 * handles client.Receives set and get request .
 * set/get keys and values in the hashtable. 
 * sends the response back to the client 
 ***********************************************************************************/
void* threadMain (void* targs)
{
	pthread_detach(pthread_self());
	//get the socket desciptor from the thread args
	int sockserv=*((int*)targs);
	char buff[BUF_SIZE];//buffer for receiving packet
	char sndbuff[BUF_SIZE];	//buffer for sending packet
	packetParam param;//for storing receved header parameters
	packetParam sParam;//for storing sending packet header parameters
	int n;
	int totalLen;

	while(1)
	{
		n=recv(sockserv,buff,sizeof(buff),0);

		if(n<=0)
		{	
			std::cout<<"connection closed\n";
			close(sockserv);
			break;
		}

		//if header is not received completely then close the connection
		if(n<24)
		{
			std::cout<<"connection closed : Corrupted data"<<buff[0]<<n<<std::endl;
			close(sockserv);
			break;
			
		}

		
		//depacketize reciev header and fill up the values		
		depacketize(buff,&param);

#ifdef DEBUG
		printf("Magic=%x opcode=%d keylength=%d TotalBodyLength=%d 				\n",param.magic,param.opCode,param.keyLength,param.totalBodyLength);
#endif

		//get value: opcode is 0x00 for get
		if(param.opCode==0x00)
		{
			int keyLength;
			unsigned char * ptr;
			std::string s2;
			keyLength=param.keyLength;
			char *pkey=( char*)malloc(sizeof(char)*keyLength);
			//copy the value of key in pkey
			memcpy(pkey,&buff[HEADER_SIZE+param.extraLength],keyLength);
			//convert key to a string for hashtable			
			std::string s1(pkey);
#ifdef DEBUG		
			std::cout<<"Server Get key:= "<<s1<<std::endl;
#endif

			pthread_mutex_lock(&rwmutex);

			//if there are active writers or waiting writers(writers given priority over readers) 
			//go to sleep on the conditional variable.Conditonal variable will release the mutex.
			//when the thread is active again it will recheck the conditon
			while(WW+AW>0)
			{
				WR++;
				pthread_cond_wait(&oktoread,&rwmutex);
				WR--;
			}
			//no active/waiting readers .start reading and increment active readers
			AR++;	
			
			pthread_mutex_unlock(&rwmutex);	

			//check whether key exist in the hash table			
			std::unordered_map<std::string,std::string>:: iterator it=hash1.find(s1);
			
			//key does not exist				
			if(it==hash1.end())
			{
#ifdef DEBUG	
				std::cout<<"key not found\n";
#endif	
				// we will fill "not found" in the response packet				
				s2="notfound";
				sParam.status=0x0001;//key not found status
			}
			//key exist.
			else
			{
				//retrive value from hash table corresponding to the key
				s2=hash1[s1];
				sParam.status=0x0002;//key exists status
			}
			
				
			

			//reading completed
			pthread_mutex_lock(&rwmutex);
			//decrement the active reader
			AR--;
			//if there are no active reader left then wake up if any waiting writer
			if((AR==0)&&(WW>0))
			{
				pthread_cond_signal(&oktowrite);
			}	
			pthread_mutex_unlock(&rwmutex);	

				
					
			//send the response packet
			memset(&sParam,0,sizeof(sParam));
			//magic byte=0x81 for response packet
			sParam.magic=0x81;
			//extra length will be 4 for get response packet
			sParam.extraLength=4;
			//fill up the total body length=value length+ extra length
			sParam.totalBodyLength=s2.size()+sParam.extraLength;
			
			//fill up the send buffer with header parameter
			packetize(sndbuff,&sParam);
			const char *cstr=s2.c_str();
			//copy the value string or "notfound in the response buffer"
			memcpy(&sndbuff[HEADER_SIZE+sParam.extraLength],cstr,strlen(cstr));
			//total length of the packet
			totalLen=HEADER_SIZE+strlen(cstr)+sParam.extraLength;
		}
		
		//set value: opcode is 0x01 for set		
		else if(param.opCode==0x01)
		{
			int keyLength;
			int valLength;
			unsigned char * ptr;
			//keylength from the header
			keyLength=param.keyLength;
			//value length calculated from the header
			valLength=param.totalBodyLength - param.keyLength - param.extraLength;
#ifdef DEBUG	
			std::cout<<"server set: keylength= "<<keyLength<<std::endl<<"server set : value length= "<<valLength<<std::endl;
#endif	

			 char *pkey=( char*)malloc(sizeof(char)*keyLength);
			//copy the key  in pkey
			memcpy(pkey,&buff[HEADER_SIZE+param.extraLength],keyLength);
		
			char *pVal=( char*)malloc(sizeof(char)*keyLength);
			//copy the value  in pval
			memcpy(pVal,&buff[HEADER_SIZE+keyLength+ param.extraLength],valLength);
		       
			//convert key and value to string 
			std::string s1(pkey);
			std::string s2(pVal);
#ifdef DEBUG		
			std::cout<<"server set: key== "<<s1<<"server set: value="<<s2<<std::endl;
#endif	
			//memset structute which stores the header param to 0			
			memset(&sParam,0,sizeof(sParam));
			//if the value is too large
			if(HEADER_SIZE+s2.size()+ 8 >BUF_SIZE)
			{
				//set the staus message:value too large
				sParam.status=0x0003;
				
				
			}
                        //add the key/value pair
			else
			{	
				pthread_mutex_lock(&rwmutex);
				//if there are active readers or waiting writers(writers given priority over readers) 
				//go to sleep on the conditional variable.Conditonal variable will release the mutex.
				//when the thread is active again it will recheck the conditon
				while(AR+AW>0)
				{
					WW++;
				
					pthread_cond_wait(&oktowrite,&rwmutex);
				
					WW--;
				}
				//no active reader/ writer .start writing and increment active writers
				AW++;	
				pthread_mutex_unlock(&rwmutex);
			
				//store/replace the pair key/value in the hash table 
				hash1[s1]=s2;
				sParam.status=0x0000;
			
			
				pthread_mutex_lock(&rwmutex);
				AW--;
				//if waiting writer . wake them up(writers given priority)
				if(WW>0)
				{
					pthread_cond_signal(&oktowrite);
				}
				//if no waiting writer. wake up all the sleeping readers
				else
				{
					pthread_cond_broadcast(&oktoread);
				}
			
				pthread_mutex_unlock(&rwmutex);	
			}
		
			//response packet will be header with status.status already set above
			sParam.magic=0x81;
			sParam.opCode=0x01;
			//fill up the header in the send buffer
			packetize(sndbuff,&sParam);
			
			//response packet will only have header
			totalLen=HEADER_SIZE;
					
		}
		else
		{
			std::cout<<"Unreconized opcode close the connection\n";
			close(sockserv);
			break;
		}
		
		//send the packet
		send(sockserv,sndbuff,totalLen,0);
	}
	
	return NULL;

		
}


/************************************************************************************
 * depacketize
 *
 * parse the header in the received buffer
 
 ***********************************************************************************/

void depacketize(void* buff,packetParam *param)
{
	unsigned char * header=(unsigned char*)buff;
	param->magic=header[0];
	param->opCode=header[1];
	param->keyLength=(header[2]<<8)|header[3];
	param->extraLength=header[4];
	param->dataType=header[5];
	param->status=(header[6]<<8)|header[7];
	param->totalBodyLength=(header[8]<<24)|(header[9]<<16)|(header[10]<<8)|(header[11]);
		
}


/************************************************************************************
 * packetize
 *
 * Fill up the header in the send  buffer
 
 ***********************************************************************************/
void packetize(void* buff,packetParam *param)
{
	unsigned char * header=(unsigned char*)buff;
	header[0]=param->magic;
	header[1]=param->opCode;
	header[2]=(param->keyLength)>>8;
	header[3]=param->keyLength;
	header[4]=param->extraLength;
	header[5]=param->dataType;
	header[6]=param->status>>8;
	header[7]=param->status;
	
	header[8]=(param->totalBodyLength>>24)&0xff;
	header[9]=(param->totalBodyLength>>16)&0xff;
	header[10]=(param->totalBodyLength>>8)&0xff;
	header[11]=(param->totalBodyLength)&0xff;
	

	

}
