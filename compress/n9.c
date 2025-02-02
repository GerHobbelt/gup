#include "encode.h"
#include "compress.h"
#include "decode.h"

#if 0
	/* log literal en pointer len combi's */
	static unsigned long log_pos_counter=0;
	#define LOG_LITERAL(lit)  {printf("%lX Literal: %02X\n", log_pos_counter, lit); log_pos_counter++;}
	#define LOG_PTR_LEN(len, ptr) {printf("%lX Len: %u, ptr: %u\n", log_pos_counter ,len, ptr); log_pos_counter+=len;}
	#define LOG_BIT(bit) // printf("bit = %i\n",bit);
  	#define LOG_RUN(run) printf("Run = %lu\n", run);
	#define LOG_COUNTER_RESET log_pos_counter=0;
	#define LOG_TEXT(string) printf(string);
#else
	#define LOG_LITERAL(lit) /* */
	#define LOG_PTR_LEN(len, ptr) /* */
	#define LOG_BIT(bit) /* */
	#define LOG_RUN(run) /* */
 	#define LOG_COUNTER_RESET
	#define LOG_TEXT(string) /* */
#endif



int n9_len_len(match_t val);
int n9_ptr_len(ptr_t val);
void n9_store_val(uint32 val, packstruct *com);
void n9_store_len_val(uint32 val, packstruct *com);
void n9_store_literal_val(uint32 val, packstruct *com);
void n9_store_ptr_val(int32_t val, packstruct *com);


unsigned long n9_cost_ptrlen(match_t match, ptr_t ptr)
{
	unsigned long res=1; /* 1 bit voor aan te geven dat het een ptr len is */
	if(match<2)
	{ /* match niet mogelijk */
		return 256;
	}
	if((match==2) && (ptr>255))
	{
		return 256;
	}
	if(ptr<256)
	{
		match++;
	}
	res+=n9_len_len(match);
	res+=n9_ptr_len(ptr);
	return res;
}

unsigned long n9_cost_lit(match_t kar)
{
	NEVER_USE(kar);
	return 9; /* 1 bit om aan te geven dat het een literal is en 8 bits voor de literal */
}


int n9_len_len(match_t val)
{ /* bereken de code lengte voor val, 2 <= val <= 2^32 */
	return 2*(first_bit_set32(val-1)-1);
}

int n9_ptr_len(ptr_t val)
{ /* bereken de code lengte voor val, 0 <= val <= 65536 */
	if(val<256)
	{
		return 9;
	}
	else if(val<65536)
	{
		return 18;
	}
	return 25;
}

#define N9_ST_BIT(bit)												\
{ /* store a 1 or a 0 */											\
	int val=bit;			                                 \
	LOG_BIT(val);														\
	if(com->bits_in_bitbuf==0)										\
	{ /* reserveer plek in bytestream */						\
		com->command_byte_ptr=com->rbuf_current++;			\
		com->bitbuf=0;													\
	}																		\
	com->bits_in_bitbuf++;											\
	com->bitbuf+=com->bitbuf+val;									\
	if (com->bits_in_bitbuf >= 8)									\
	{																		\
		com->bits_in_bitbuf=0;										\
		*com->command_byte_ptr=(uint8)com->bitbuf;			\
	}																		\
}

void n9_store_val(uint32 val, packstruct *com)
{ /* waarde val >=2 */
	int bits_to_do=first_bit_set32(val)-1;
	uint32 mask=1<<bits_to_do;
	mask>>=1;
	do
	{
		if((val&mask)==0)
		{
			N9_ST_BIT(0);
		}
		else
		{
			N9_ST_BIT(1);
		}
		mask>>=1;
		if(mask==0)
		{
			N9_ST_BIT(1);
		}
		else
		{
			N9_ST_BIT(0);
		}
	}while(mask!=0);
}

void n9_store_len_val(uint32 val, packstruct *com)
{ /* waarde val >=3 */
	n9_store_val(val-1, com);
}

void n9_store_ptr_val(int32_t val, packstruct *com)
{ /* waarde val >=0 <=65535 */
	val++;
	if(val<=256)
	{
		val=-val;
		*com->rbuf_current++ = (uint8) (val&0xff);
		N9_ST_BIT(0);
	}
	else if(val<=65536)
	{
		val=-val;
		*com->rbuf_current++ = (uint8) ~((val>>8)&0xff);
		N9_ST_BIT(1);
		*com->rbuf_current++ = (uint8) (val&0xff);
		N9_ST_BIT(0);
	}
	else
	{
		val=-val;
		*com->rbuf_current++ = (uint8) ~((val>>16)&0xff);
		N9_ST_BIT(1);
		*com->rbuf_current++ = (uint8) ((val>>8)&0xff);
		N9_ST_BIT(1);
		*com->rbuf_current++ = (uint8) (val&0xff);
	}
}

gup_result n9_compress(packstruct *com)
{
	/*
	** pointer lengte codering:
	** 8 xxxxxxxx0              0 -   255   9 bits
	** 16 xxxxxxxx1xxxxxxxx   256 - 65536  17 bits
	**
	** len codering:
	** 1 x0              :  2 -   3   2 bits
	** 2 x1x0            :  4 -   7   4 bits
	** 3 x1x1x0          :  8 -  15   6 bits
	** 4 x1x1x1x0        : 16 -  31   8 bits
	** 5 x1x1x1x1x0      : 32 -  63  10 bits
	** 6 x1x1x1x1x1x0    : 64 - 127  12 bits
	** 7 x1x1x1x1x1x1x0  :128 - 255  14 bits
	** enz...
	**   
	*/
	index_t current_pos = DICTIONARY_START_OFFSET; /* wijst de te packen byte aan */
	unsigned long bytes_to_do=com->origsize;
	void* rbuf_current_store=com->rbuf_current;
	com->rbuf_current=com->compressed_data;
	com->bits_in_bitbuf=0;
	{
		match_t kar;
		kar=com->dictionary[current_pos];
		LOG_LITERAL(kar);
		*com->rbuf_current++=(uint8)kar;
		bytes_to_do--;
		current_pos++;
	}
	while(bytes_to_do>0)
	{
		match_t match;
		match=com->match_len[current_pos];
		if(match==0)
		{ /* store literal */
			match_t kar;
			N9_ST_BIT(0);
			kar=com->dictionary[current_pos];
			LOG_LITERAL(kar);
			*com->rbuf_current++=(uint8)kar;
			bytes_to_do--;
			current_pos++;
		}
		else
      {
      	ptr_t ptr;
			N9_ST_BIT(1);
			ptr=com->ptr_len[current_pos];
         LOG_PTR_LEN(match, ptr+1);
         bytes_to_do-=match;
         current_pos+=match;
         n9_store_ptr_val(ptr, com);
         if(ptr<256)
         {
         	match++;
         }
         if(ptr>65535)
         {
         	match--;
         }
         n9_store_len_val(match, com);
		}
	}
	{ /* schrijf n9 end of file marker */
		N9_ST_BIT(1); /* pointer comming */
		*com->rbuf_current++ = 0; 
		N9_ST_BIT(1); /* deze combi kan niet voorkomen */
	}
	if (com->bits_in_bitbuf>0)
	{
		com->bitbuf=com->bitbuf<<(8-com->bits_in_bitbuf);
		*com->command_byte_ptr=(uint8)com->bitbuf;
		com->bits_in_bitbuf=0;
	}
	com->command_byte_ptr=NULL;
	com->packed_size=com->rbuf_current-com->compressed_data;
	com->bytes_packed=com->origsize;
	com->rbuf_current=rbuf_current_store;
	{
		unsigned long bytes_to_do=com->packed_size;
		uint8 *src=com->compressed_data;
		while(bytes_to_do>0)
		{
			unsigned long bytes_comming=65536;
			gup_result res;
			if(bytes_comming>bytes_to_do)
			{
				bytes_comming=bytes_to_do;
			}
			if((res=announce(bytes_comming, com))!=GUP_OK)
			{
				return res;
			}
			bytes_to_do-=bytes_comming;
			while(bytes_comming-->0)
			{
				*com->rbuf_current++=*src++;
			}	
		}
	}
	return GUP_OK;
}

gup_result n9_close_stream(packstruct *com)
{
	NEVER_USE(com);
	return GUP_OK;
}


#define N9_GET_BIT(bit)								\
{ /* get a bit from the data stream */			\
 	if(bits_in_bitbuf==0)							\
 	{ /* fill bitbuf */								\
  		if(com->rbuf_current>com->rbuf_tail)	\
		{													\
			gup_result res;							\
			if((res=read_data(com))!=GUP_OK)		\
			{												\
				return res;								\
			}												\
		}													\
  		bitbuf=*com->rbuf_current++;				\
  		bits_in_bitbuf=8;								\
	}														\
	bit=(bitbuf&0x80)>>7;							\
	bitbuf+=bitbuf;									\
	bits_in_bitbuf--;									\
}

#define N9_DECODE_LEN(val)							\
{ /* get value 2 - 2^32-1 */						\
	int bit;												\
	val=1;												\
	do														\
	{														\
		N9_GET_BIT(bit);								\
		val+=val+bit;									\
		N9_GET_BIT(bit);								\
	} while(bit==0);									\
}


gup_result n9_decode(decode_struct *com)
{
	uint8* dst=com->buffstart;
	uint8* dstend;
	uint8 bitbuf=0;
	int bits_in_bitbuf=0;
	dstend=com->buffstart+65536L;
	if(com->origsize==0)
	{
		return GUP_OK; /* exit succes? */
	}
	{ /* start met een literal */
		com->origsize--;
  		if(com->rbuf_current > com->rbuf_tail)
		{
			gup_result res;
			if((res=read_data(com))!=GUP_OK)
			{
				return res;
			}
		}
		LOG_LITERAL(*com->rbuf_current);
		*dst++=*com->rbuf_current++;
		if(dst>=dstend)
		{
			gup_result err;
			long bytes=dst-com->buffstart;
			com->print_progres(bytes, com->pp_propagator);
			if ((err = com->write_crc(bytes, com->buffstart, com->wc_propagator))!=GUP_OK)
			{
				return err;
			}
			dst-=bytes;
			memmove(com->buffstart-bytes, com->buffstart, bytes);
		}
	}
	for(;;)
	{
		int bit;
		N9_GET_BIT(bit);
		if(bit==0)
		{ /* literal */
	  		if(com->rbuf_current > com->rbuf_tail)
			{
				gup_result res;
				if((res=read_data(com))!=GUP_OK)
				{
					return res;
				}
			}
			LOG_LITERAL(*com->rbuf_current);
			*dst++=*com->rbuf_current++;
			if(dst>=dstend)
			{
				gup_result err;
				long bytes=dst-com->buffstart;
				com->print_progres(bytes, com->pp_propagator);
				if ((err = com->write_crc(bytes, com->buffstart, com->wc_propagator))!=GUP_OK)
				{
					return err;
				}
				dst-=bytes;
				memmove(com->buffstart-bytes, com->buffstart, bytes);
			}
		}
		else
		{ /* ptr len */
			int32 ptr;
			uint8* src;
			uint8 data;
			int len;
			ptr=-1;
			ptr<<=8;
	  		if(com->rbuf_current > com->rbuf_tail)
			{
				gup_result res;
				if((res=read_data(com))!=GUP_OK)
				{
					return res;
				}
			}
			data=*com->rbuf_current++;
			N9_GET_BIT(bit);
			if(bit==0)
			{
				ptr|=data;
			}
			else
			{ /* 16 bit pointer */
				if(data==0)
				{
					break; /* end of stream */
				}
				ptr|=~data;
				ptr<<=8;
		  		if(com->rbuf_current > com->rbuf_tail)
				{
					gup_result res;
					if((res=read_data(com))!=GUP_OK)
					{
						return res;
					}
				}
				ptr|=*com->rbuf_current++;
			}
			N9_DECODE_LEN(len);
			len++;
			LOG_PTR_LEN(len, -ptr)
			src=dst+ptr;
			do
			{
  				*dst++=*src++;
  				if(dst>=dstend)
  				{
					gup_result err;
					long bytes=dst-com->buffstart;
					com->print_progres(bytes, com->pp_propagator);
  					if ((err = com->write_crc(bytes, com->buffstart, com->wc_propagator))!=GUP_OK)
  					{
  						return err;
  					}
					dst-=bytes;
					src-=bytes;
					memmove(com->buffstart-bytes, com->buffstart, bytes);
				}
			} while(--len!=0);
		}
	}
	{
		unsigned long len;
		if((len=(dst-com->buffstart))!=0)
		{
			gup_result err;
			com->print_progres(len, com->pp_propagator);
			if ((err = com->write_crc(len, com->buffstart, com->wc_propagator))!=GUP_OK)
			{
				return err;
			}
		}
	}
	return GUP_OK; /* exit succes */
}
