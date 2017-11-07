//
//  Copyright 2017 Marco Giorgini All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

//  -----------------------------------------------------------------------
//  Not a single line of this source code is anyhow related to the author's
//  daily work - nor this source code reflects in anyway that company 
//  technology, or source libraries
//  -----------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <io.h>

// --------------------------------------------------------------------

#define fileformat_raw      1
#define fileformat_conllu   2

#define builtin_max_word_len 256

#define filter_digits       1 
#define filter_punct        2

// --------------------------------------------------------------------

size_t max_word_len=60;

// --------------------------------------------------------------------
//
// String Dictionary implementation (add&search only)
// with its own heap and counters for TF-IDF 
//
// --------------------------------------------------------------------

// --------------------------------------------------------------------
//
// memorybag struct
//
// A very simple heap implementation to keep faster adding words in
// dictionary. 
// Good, but using this you won't be able to easily change/remove existing
// string in the dictionary (we don't want to do that, we're going to 
// prune dictionary just at the end of the insertion task - using td-idf)
//
// --------------------------------------------------------------------

typedef struct {
 unsigned char *items;
 size_t         num,size; 
}membag;

typedef struct {
 membag        *items;
 size_t        num,size; 
}memorybag;

void memorybag_new(memorybag*mem)
{
 memset(mem,0,sizeof(*mem));
 mem->size=16;
 mem->items=(membag*)calloc(mem->size,sizeof(mem->items[0]));
 if(mem->items)
  {
   mem->items[mem->num].size=64*1024; 
   mem->items[mem->num].items=(unsigned char*)calloc(mem->items[mem->num].size,sizeof(unsigned char));
  }
}

void memorybag_delete(memorybag*mem)
{
 unsigned int i;
 for(i=0;i<mem->num;i++)
  if(mem->items[i].items)
   free(mem->items[i].items);
}

void*memorybag_alloc(memorybag*mem,unsigned int size)
{
 void*m;
 if(mem->items[mem->num].num+size>=mem->items[mem->num].size)
  {
   mem->num++;
   if(mem->num>=mem->size)
    {
     mem->size+=16;
     mem->items=(membag*)realloc(mem->items,mem->size*sizeof(membag));
    } 
   mem->items[mem->num].num=0;
   mem->items[mem->num].size=64*1024; 
   mem->items[mem->num].items=(unsigned char*)calloc(mem->items[mem->num].size,sizeof(unsigned char));   
  } 
 m=(void*)(mem->items[mem->num].items+mem->items[mem->num].num);
 mem->items[mem->num].num+=size;
 return m;
}

char*memorybag_strdup(memorybag*mem,const char*str)
{
 unsigned int len=strlen(str)+1;
 char        *s=memorybag_alloc(mem,len);
 if(s) strcpy(s,str);
 return s; 
}

// --------------------------------------------------------------------
//
// tfidf_lemma & tfidf_dict
// a simple but quick implementation of a string dictionary, able to
// count pure frequency of lemmas along with presence-in-document count
//
// tfidf_dict*tfidf_dict_new(size_t size,int granularity);
// void tfidf_dict_delete(tfidf_dict*h)
//
// tfidf_lemma*tfidf_dict_find(tfidf_dict*h,const char*lemma)
// tfidf_lemma*tfidf_dict_add(tfidf_dict*h,const char*lemma,int docid,int cnt)
//
// void tfidf_dict_sort(tfidf_dict*h,tfidf_dict_compare customtfidf_dict_compare)
//
// int tfidf_dict_import(tfidf_dict*h,const char*fn)
// int tfidf_dict_export(tfidf_dict*h,const char*fn,
//                       int what/* 1 cnt | 2 doccnt | 4 tf | 8 idf | 16 tf*idf*/,
//                       size_t cntcut/* 0 no cut, else cnt limit*/)
//
// --------------------------------------------------------------------

typedef struct {
 const char*str;
 int        docid;
 size_t     cnt,doccnt;
 float      tfidf;
}tfidf_lemma;

typedef struct {
 size_t       num,size;
 int          granularity;
 tfidf_lemma *items;
 
 size_t       hsize;
 unsigned int*hitems;
 
 memorybag    heap;
 
 int          docid;
 size_t       lemmas_cnt,docs_cnt;
}tfidf_dict;

#define DJB2

unsigned int string_hashfunct(const char*str)
{
// ------------------------------------------
// djb2
// ------------------------------------------
#if defined(DJB2)
 unsigned long hash = 5381;
 int           c;
 while (c = (unsigned char)*str++)
   hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
 return hash;
#endif 
// ------------------------------------------
// sdbm
// ------------------------------------------
#if defined(SDBM)
 unsigned long hash = 0;
 int c;
 while (c = *str++)
     hash = c + (hash << 6) + (hash << 16) - hash;
 return hash;
#endif 
}

tfidf_dict*tfidf_dict_new(size_t size,int granularity,int useheap)
{
 tfidf_dict*h=(tfidf_dict*)calloc(1,sizeof(tfidf_dict));
 if(h)
  {
   h->num=0;
   h->size=size;
   h->granularity=granularity;
   h->items=(tfidf_lemma*)calloc(h->size,sizeof(h->items[0]));   

   h->hsize=size*13-17;
   h->hitems=(unsigned int*)malloc(h->hsize*sizeof(h->hitems[0]));
   if(h->hitems) memset(h->hitems,0xFF,h->hsize*sizeof(h->hitems[0]));
   
   h->docid=-1;
   h->lemmas_cnt=h->docs_cnt=0;
   
   memorybag_new(&h->heap);
  }
 return h;  
}

void tfidf_dict_settfidf(tfidf_dict*dict)
{
 size_t i,maxcnt=dict->items[0].cnt,maxdoccnt=dict->items[0].doccnt;
 for(i=1;i<dict->num;i++)
  {
   if(dict->items[i].cnt>maxcnt)
    maxcnt=dict->items[i].cnt;
   if(dict->items[i].doccnt>maxdoccnt)
    maxdoccnt=dict->items[i].doccnt; 
  }  
 for(i=0;i<dict->num;i++)     
  {
   double tf=((double)dict->items[i].cnt/(double)maxcnt/*dict->lemmas_cnt*/);
   double idf=((double)logf((double)maxdoccnt/*dict->docs_cnt*//(double)(1+dict->items[i].doccnt)));
   dict->items[i].tfidf=(float)(tf*idf);
  }     
}

void tfidf_dict_delete(tfidf_dict*h)
{
 memorybag_delete(&h->heap);
 free(h->hitems);
 free(h->items);
 free(h);
}

int tfidf_dict_stringcompare(const void*a,const void*b)
{
 return strcmp(((tfidf_lemma*)a)->str,((tfidf_lemma*)b)->str);
}

int tfidf_dict_tfidfcompare(const void*a,const void*b)
{
 float dif=((tfidf_lemma*)b)->tfidf-((tfidf_lemma*)a)->tfidf;
 if(dif==0) return 0;
 else
 if(dif>0)  return 1;
 else       return -1;
}

typedef int (*tfidf_dict_compare)(const void*a,const void*b);

void tfidf_dict_rehash(tfidf_dict*h)
{
 size_t i;
 memset(h->hitems,0xFF,h->hsize*sizeof(h->hitems[0]));
 for(i=0;i<h->num;i++)
  {
   unsigned int hi=string_hashfunct(h->items[i].str)%h->hsize;
   while(h->hitems[hi]!=-1)
    hi=(hi+1)%h->hsize;
   h->hitems[hi]=i;
  } 
}

void tfidf_dict_sort(tfidf_dict*h,tfidf_dict_compare customtfidf_dict_compare)
{ 
 qsort(h->items,h->num,sizeof(h->items[0]),customtfidf_dict_compare);
 tfidf_dict_rehash(h);
}

int tfidf_dict_export(tfidf_dict*h,
                      const char*fn,
                      int what/* 1 cnt | 2 doccnt | 4 tf | 8 idf | 16 tf*idf*/,
                      size_t cntcut/* 0 no cut, else cnt limit*/,
                      size_t doccntcut/* 0 no cut, else doccnt limit*/
                     )
{
 int  cnt=0;
 FILE*f=fopen(fn,"wb+");
 if(f)
  {
   size_t i;
   fprintf(f,"# lemma");
   if(what&1)
    fprintf(f,"\tcount(%d)",h->lemmas_cnt);
   if(what&2)
    fprintf(f,"\tdoccount(%d)",h->docs_cnt);    
   if(what&4)
    fprintf(f,"\tTFxIDF");        
   fprintf(f,"\r\n");
   for(i=0;i<h->num;i++)
    if((h->items[i].cnt<=cntcut)||((doccntcut>256)&&(h->items[i].doccnt<=doccntcut)))
     ;
    else 
    if((h->docs_cnt>1)&&(h->items[i].tfidf<=0.0f))
     ;
    else 
     {      
      fprintf(f,"%s",h->items[i].str);
      if(what&1)
       fprintf(f,"\t%d",h->items[i].cnt);
      if(what&2)
       fprintf(f,"\t%d",h->items[i].doccnt);       
      if(what&4)
       fprintf(f,"\t%.4f",h->items[i].tfidf);                            
      fprintf(f,"\r\n");      
      cnt++;
     } 
   fclose(f);  
  }
 return cnt; 
}

tfidf_lemma*tfidf_dict_find(tfidf_dict*h,const char*lemma)
{
 unsigned int i=string_hashfunct(lemma)%h->hsize;
 while(h->hitems[i]!=-1)
  if(strcmp(h->items[h->hitems[i]].str,lemma)==0)
   return &h->items[h->hitems[i]];
  else 
   i=(i+1)%h->hsize;
 return NULL;  
}

void tfidf_dict_updateglobalstats(tfidf_dict*h,int docid,int cnt)
{
 if(h->docid!=docid)
  {
   h->docid=docid;
   h->docs_cnt++;
  }
 h->lemmas_cnt+=cnt;
}

tfidf_lemma*tfidf_dict_add(tfidf_dict*h,const char*lemma,int docid,int cnt)
{
 unsigned int i=string_hashfunct(lemma)%h->hsize,miss=0;
 while(h->hitems[i]!=-1)
  if(strcmp(h->items[h->hitems[i]].str,lemma)==0)
   {
    int hi=h->hitems[i];
    if(h->items[hi].docid!=docid)
     {
      h->items[hi].docid=docid;
      h->items[hi].doccnt++;
     }
    h->items[hi].cnt+=cnt;
    tfidf_dict_updateglobalstats(h,docid,cnt);
    return &h->items[hi];
   } 
  else
   {i=(i+1)%h->hsize;miss++;}
 
 if(h->hitems[i]==-1) 
  {
   h->hitems[i]=h->num;
   if(h->num>=h->size)
    {
     h->size+=h->granularity;
     h->items=(tfidf_lemma*)realloc(h->items,h->size*sizeof(tfidf_lemma));
    }
   if(h->num>h->hsize/2)
    {
     h->hsize=h->num*13-17;
     h->hitems=(int*)realloc(h->hitems,h->hsize*sizeof(h->hitems[0]));
     tfidf_dict_rehash(h);
    }
   h->items[h->num].str=memorybag_strdup(&h->heap,lemma);
   h->items[h->num].docid=docid;
   h->items[h->num].doccnt=1;
   h->items[h->num].cnt=cnt;
   tfidf_dict_updateglobalstats(h,docid,cnt);
   return &h->items[h->num++];
  }
 else 
  return NULL; 
}

int tfidf_dict_import(tfidf_dict*h,const char*fn)
{
 FILE*f=fopen(fn,"rb");
 if(f)
  {
   char   line[2048];
   size_t hm=0;
   while(!feof(f))
    {
     size_t i=0,lcnt=0,dcnt=0;     
     fgets(line,sizeof(line)-1,f);
     if((hm==0)&&(memcmp(line,"# lemma",7)==0))
      continue;
     hm++; 
     while((i<sizeof(line)-1)&&(line[i]!='\t')&&(line[i]!='\r')&&(line[i]!='\n')&&(line[i]!=0))
      i++;
     while(line[i]=='\t') 
      {
       char   cnt[32];
       size_t j=0;
       line[i++]=0; 
       while((i<sizeof(line)-1)&&(line[i]!='\t')&&(line[i]!='\r')&&(line[i]!='\n')&&(line[i]!=0))
        if(j<sizeof(cnt)-1)
         cnt[j++]=line[i++];
        else 
         i++;
       cnt[j]=0;  
       if(lcnt==0)
        lcnt=atoi(cnt);
       else 
       if(dcnt==0)
        dcnt=atoi(cnt);
       else
        break; 
      } 
     line[i]=0; 
     if(*line)
      {
       tfidf_lemma*lm=tfidf_dict_add(h,line,1,1);        
       if(lm&&lcnt&&dcnt)
        {
         lm->cnt=lcnt;
         lm->doccnt=dcnt;
        }
      } 
    }
   fclose(f);  
  }
 return h->num; 
}

// --------------------------------------------------------------------
//
// hquad
// an ibrid normal/associative 2d array to store huge <x,y>=cnt data
// used to store corpus elements for each dictionary item
//
// --------------------------------------------------------------------

typedef struct {
 unsigned int   coord;
 unsigned int   cnt;
}hashquad;

typedef struct {
 int        num,size;
 hashquad  *items;
}hashquads;

unsigned int hashquadfunct( unsigned int a)
{
   a = (a+0x7ed55d16) + (a<<12);
   a = (a^0xc761c23c) ^ (a>>19);
   a = (a+0x165667b1) + (a<<5);
   a = (a+0xd3a2646c) ^ (a<<9);
   a = (a+0xfd7046c5) + (a<<3);
   a = (a^0xb55a4f09) ^ (a>>16);
   return a;
}

int hashquads_new(hashquads*h,int size)
{ 
 h->num=0;
 h->size=size/*8803*/;
 h->items=(hashquad*)calloc(h->size,sizeof(h->items[0]));
 if(h->items==NULL)
  {
   int requestedmem=size*sizeof(h->items[0]);
   return 0;
  } 
 else
  return 1; 
}

void hashquads_delete(hashquads*h)
{
 if(h->items)
  free(h->items);
}

hashquad*hashquads_find(hashquads*h,unsigned int coord)
{
 unsigned int i=hashquadfunct(coord)%h->size;
 while(h->items[i].cnt!=0)
  if(h->items[i].coord==coord)
   return &h->items[i];
  else 
   i=(i+1)%h->size;
 return NULL;  
}

hashquad*hashquads_addex(hashquads*h,hashquad*hl)
{
 unsigned int i=hashquadfunct(hl->coord)%h->size;
 while(h->items[i].cnt!=0)
  if(h->items[i].coord==hl->coord)
   {
    h->items[i].cnt=hl->cnt;
    return &h->items[i];
   } 
  else
   i=(i+1)%h->size;
 
 if(h->items[i].cnt==0) 
  {
   h->num++;
   h->items[i].coord=hl->coord;
   h->items[i].cnt=hl->cnt;
   return &h->items[i];
  }
 else 
  return NULL; 
}

hashquad*hashquads_add(hashquads*h,unsigned int coord,int cnt,int*newslot)
{
 unsigned int i=hashquadfunct(coord)%h->size,miss=0;
 while(h->items[i].cnt!=0)
  if(h->items[i].coord==coord)
   {
    h->items[i].cnt+=cnt;
    return &h->items[i];
   } 
  else
   {i=(i+1)%h->size;miss++;}
 
 if(h->items[i].cnt==0) 
  {
   h->num++;
   if((miss>1024)||(h->num>h->size-17))
    {          
     hashquads nh;
     int       nsize;
     if(h->size<65535)
      nsize=h->size*2-17;
     else 
      nsize=h->size+h->size/7-17;
     if(hashquads_new(&nh,nsize))
      {
       int       i;
       hashquad* rid;
       for(i=0;i<h->size;i++)
        if(h->items[i].cnt)
         hashquads_addex(&nh,&h->items[i]);
       free(h->items);
       rid=hashquads_add(&nh,coord,cnt,newslot);       
       h->items=nh.items;
       h->num=nh.num;
       h->size=nh.size;
       return rid;
      }
     else
      return NULL;  
    }
   else
    {          
     h->items[i].coord=coord;
     h->items[i].cnt=cnt;
     if(newslot) *newslot=1;
     return &h->items[i];
    } 
  }
 else 
  return NULL; 
}

typedef struct {
 int            w,h;
 unsigned short size;
 int            used;
 hashquads**q;
}hquad;

void hquad_new(hquad*hq,unsigned short size,int width,int height)
{
 int y;
 hq->used=0;
 hq->size=size;
 hq->w=((width-1)/hq->size)+1;
 hq->h=((height-1)/hq->size)+1;
 hq->q=calloc(hq->h,sizeof(hashquads*));
 for(y=0;y<hq->h;y++)
  hq->q[y]=(hashquads*)calloc(hq->w,sizeof(hashquads));
}

void hquad_delete(hquad*hq)
{
 int x,y;
 for(y=0;y<hq->h;y++)
  {
   for(x=0;x<hq->w;x++)
    if(hq->q[y][x].size)
     hashquads_delete(&hq->q[y][x]);
   free(hq->q[y]);
  } 
 free(hq->q); 
}

int hquad_set(hquad*hq,int x,int y,int value,int way)
{ 
 int qx=x/hq->size,qy=y/hq->size;
 if((qx>=0)&&(qx<=hq->w-1)&&(qy>=0)&&(qy<=hq->h-1))
  {
   int      rx=x%hq->size,ry=y%hq->size,newslot=0;
   hashquad*item;
   if(hq->q[qy][qx].size==0)  hashquads_new(&hq->q[qy][qx],683);
   item=hashquads_add(&hq->q[qy][qx],(rx|(ry<<16)),value,&newslot);
   if(item==NULL)
    return -1;
   if(newslot)
    hq->used++;
   return 1; 
  }
 else
  return 0; 
}

int hashquad_compare(const void*a,const void*b)
{
 hashquad*A=(hashquad*)a;
 hashquad*B=(hashquad*)b;
 int      xa,xb;
 if(A->cnt==0) xa=0x7FFFFFFF;
 else          xa=A->coord>>16;
 if(B->cnt==0) xb=0x7FFFFFFF;
 else          xb=B->coord>>16;
 if(xa-xb)     return xa-xb;
 else          return B->cnt-A->cnt; 
}

int hashquad_simplecompare(const void*a,const void*b)
{
 hashquad*A=(hashquad*)a;
 hashquad*B=(hashquad*)b;
 int      xa,xb;
 xa=A->coord>>16;
 xb=B->coord>>16;
 return xa-xb;
}

int id_compare(const void*a,const void*b)
{
 return (*(int*)a)-(*(int*)b);
}

int cnt_compare(const void*a,const void*b)
{
 return ((int*)b)[1]-((int*)a)[1];
}

int hquad_reduce(hquad*hq,size_t cut)
{
 int x,y,red=0;
 for(y=0;y<hq->h;y++)
  for(x=0;x<hq->w;x++)
   if(hq->q[y][x].items)
    {
     int j;
     for(j=0;j<hq->q[y][x].size;j++)
      if(hq->q[y][x].items[j].cnt)
       if(hq->q[y][x].items[j].cnt<=cut)
        {
         hq->q[y][x].items[j].cnt=hq->q[y][x].items[j].coord=0;
         hq->q[y][x].num--;
         hq->used--;red++;
        } 
    }
 return red;   
}

void hquad_setreadonlymode(hquad*hq)
{
 int x,y;
 for(y=0;y<hq->h;y++)
  for(x=0;x<hq->w;x++)
   if(hq->q[y][x].items)
    {
     int j;
     qsort(hq->q[y][x].items,hq->q[y][x].size,sizeof(hq->q[y][x].items[0]),hashquad_compare);
     for(j=0;j<hq->q[y][x].size;j++)
      if(hq->q[y][x].items[j].cnt==0)
       {
        hq->q[y][x].num=j;
        break;
       }
    }
}

int hquad_writebinary(hquad*hq,const char*bin)
{
 FILE*f=fopen(bin,"wb+");
 if(f)
  {
   int x,y,num=0,err=0;
   if(fwrite("HQUA",1,4,f)!=4)                                  err++;
   if(fwrite(&hq->w,1,sizeof(hq->w),f)!=sizeof(hq->w))          err++;
   if(fwrite(&hq->h,1,sizeof(hq->h),f)!=sizeof(hq->h))          err++;
   if(fwrite(&hq->size,1,sizeof(hq->size),f)!=sizeof(hq->size)) err++;
   if(fwrite(&hq->used,1,sizeof(hq->used),f)!=sizeof(hq->used)) err++;
   for(y=0;y<hq->h;y++)
    for(x=0;x<hq->w;x++)
     if(hq->q[y][x].items)
      {
       if(fwrite(&hq->q[y][x].num,1,sizeof(hq->q[y][x].num),f)!=sizeof(hq->q[y][x].num)) err++;
       if(hq->q[y][x].num)
        if(fwrite(hq->q[y][x].items,1,hq->q[y][x].num*sizeof(hq->q[y][x].items[0]),f)!=hq->q[y][x].num*sizeof(hq->q[y][x].items[0]))
         err++;
      }
     else 
      if(fwrite(&num,1,sizeof(num),f)!=sizeof(num)) err++;
   fclose(f);
   return (err==0);   
  }    
 else
  return 0; 
}

size_t hquad_getreadonlyrow(hquad*hq,int y,int*row,int maxelements)
{
 int    qy=y/hq->size;
 size_t cnt=0;
 if((qy>=0)&&(qy<=hq->h-1))
  {
   int x;
   for(x=0;x<hq->w;x++)
    if(hq->q[qy][x].size)
     {
      int       hm=hq->q[qy][x].num,offset=qy*hq->size;
      hashquad *items=hq->q[qy][x].items;
      hashquad  search,*fnd;
      search.coord=(y-offset)<<16;
      fnd=(hashquad*)bsearch(&search,items,hm,sizeof(items[0]),hashquad_simplecompare);
      if(fnd)
       {
        int j=fnd-items;
        while(j&&((items[j-1].coord&0xFFFF0000)==search.coord)) j--;
        while((j<hm)&&((items[j].coord&0xFFFF0000)==search.coord))
         {
          row[cnt++]=(items[j].coord&0xFFFF)+x*hq->size;
          row[cnt++]=items[j].cnt;
          if((maxelements!=-1)&&(cnt/2>=(size_t)maxelements))
           return cnt/2;
          j++;
         }
       }  
     }
  }
 return cnt/2;
}

int hquad_writetext(hquad*hq,tfidf_dict*dict,const char*text,int neighborhoodsize)
{
 FILE*f=fopen(text,"wb+");
 if(f)
  {
   size_t x,y;
   int*row=(int*)calloc(neighborhoodsize*2,sizeof(int));
   for(y=0;y<dict->num;y++)     
    {
     size_t rowcnt=hquad_getreadonlyrow(hq,y,row,neighborhoodsize);
     if(rowcnt) 
      {
       const char*szx=dict->items[y].str;
       fprintf(f,"%s: ",szx);
       for(x=0;x<rowcnt;x++)
        {
         const char*szy=dict->items[row[x*2]].str;
         int        cnt=row[x*2+1];
         if(x)
          fprintf(f,", %s_%d",szy,cnt);
         else
          fprintf(f,"%s_%d",szy,cnt);
        } 
       fprintf(f,"\r\n");  
      } 
     if((y%1024)==0)
      printf("emit: %d   \r",y);        
    }
   free(row); 
   fclose(f);   
   return 1;   
  }   
 else
  return 0; 
}      

int hquad_readbinary(hquad*hq,const char*bin)
{
 FILE*f=fopen(bin,"rb");
 if(f)
  {   
   char magic[4];
   int  ret=1,num=0;
   if((fread(magic,1,4,f)==4)&&(memcmp(magic,"HQUA",4)==0))
    {
     int    x,y;     
     size_t read;
     if(fread(&hq->w,1,sizeof(hq->w),f)!=sizeof(hq->w)) 
      ret=0;
     else 
     if(fread(&hq->h,1,sizeof(hq->h),f)!=sizeof(hq->h))
      ret=0;
     else 
     if(fread(&hq->size,1,sizeof(hq->size),f)!=sizeof(hq->size))
      ret=0;
     else 
     if(fread(&hq->used,1,sizeof(hq->used),f)!=sizeof(hq->used))
      ret=0; 
     else
      { 
       hq->q=calloc(hq->h,sizeof(hashquads*));
       for(y=0;y<hq->h;y++)
        hq->q[y]=(hashquads*)calloc(hq->w,sizeof(hashquads));
       for(y=0;(y<hq->h)&&ret;y++)
        for(x=0;(x<hq->w)&&ret;x++)
         if(fread(&num,1,sizeof(num),f)!=sizeof(num))
          ret=0;
         else 
          if(num)
           {
            hq->q[y][x].size=hq->q[y][x].num=num;
            hq->q[y][x].items=(hashquad*)malloc(num*sizeof(hq->q[y][x].items[0]));
            if((read=fread(hq->q[y][x].items,1,num*sizeof(hq->q[y][x].items[0]),f))!=num*sizeof(hq->q[y][x].items[0]))
             ret=0;
           }         
      }   
    }  
   else
    ret=0; 
   fclose(f);
   return ret;   
  }    
 else
  return 0; 
}

int hquad_get(hquad*hq,int x,int y)
{
 int qx=x/hq->size,qy=y/hq->size;
 if((qx>=0)&&(qx<=hq->w-1)&&(qy>=0)&&(qy<=hq->h-1))
  {
   int rx=x%hq->size,ry=y%hq->size;
   if(hq->q[qy][qx].size==0)
    return 0;
   else
    { 
     hashquad*item=hashquads_find(&hq->q[qy][qx],(rx|(ry<<16)));
     if(item)
      return item->cnt;
     else
      return 0; 
    }  
  }
 else
  return 0;  
}

// --------------------------------------------------------------------


// --------------------------------------------------------------------

int getparam(char*request,int argc,char*argv[],char*param)
{
 int i;
 for(i=1;i<argc;i++)
  if(strcmp(argv[i],request)==0)
   {
    if(param)
     if(i+1<argc)
      strcpy(param,argv[i+1]);
     else
      *param=0; 
    return 1;
   }
 return 0;
}

void setextension(char*file,const char*ext)
{
 int l=strlen(file);
 while(l--)
  if(file[l]=='.')
   {
    strcpy(file+l+1,ext);
    return;
   }
  else
  if((file[l]=='\\')||(file[l]=='/')) 
   { 
    strcat(file,".");strcat(file,ext);
    return;
   }
 strcat(file,".");strcat(file,ext);  
}

void removeendingcrlf(char*line)
{
 size_t l=strlen(line);
 while(l&&((line[l-1]=='\n')||(line[l-1]=='\r')))
  line[--l]=0;       
}

const char*gettoken(const char*s,char*out,int outsize,char sep)
{
 int o=0;
 while(*s&&(*s!=sep))
  if(out)
   {
    if(o<outsize)
     out[o++]=*s++;
    else
     s++;
   } 
  else
   s++;
 if(out)  
  out[o]=0;
 if(*s)
  return s+1;
 else
  return NULL; 
}

// --------------------------------------------------------------------

// Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.

#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static const unsigned char utf8d[] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
  8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
  0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
  0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
  0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
  1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
  1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
  1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

unsigned int decode(unsigned int* state, unsigned int* codep, unsigned int byte) {
  unsigned int type = utf8d[byte];

  *codep = (*state != UTF8_ACCEPT) ?
    (byte & 0x3fu) | (*codep << 6) :
    (0xff >> type) & (byte);

  *state = utf8d[256 + *state*16 + type];
  return *state;
}

unsigned int validate_utf8(unsigned int *state, char *str, size_t len)
{
   size_t i;
   unsigned int type;
   for (i = 0; i < len; i++) {
        type = utf8d[(unsigned char)str[i]];
        *state = utf8d[256 + (*state) * 16 + type];
        if (*state == UTF8_REJECT)
            break;
    }
    return *state;
}

// --------------------------------------------------------------------

int file_checkutf(FILE*f)
{
 char         buf[256];
 size_t       bytes_read;
 unsigned int state=UTF8_ACCEPT;
 bytes_read=fread(buf,1,sizeof(buf),f);
 validate_utf8(&state,buf,bytes_read);
 if(state==UTF8_ACCEPT)
  {
   if((bytes_read>=3)&&((unsigned char)buf[0]==239)&&((unsigned char)buf[1]==187)&&((unsigned char)buf[2]==191))
    fseek(f,3,SEEK_SET);
   else
    fseek(f,0,SEEK_SET);
   return 1; 
  }
 else
  {
   fseek(f,0,SEEK_SET);
   return 0; 
  } 
}

// --------------------------------------------------------------------

void fgetc_read(FILE*f,unsigned char*seq,int cnt)
{
 int i=0;
 while(cnt--)
  if(!feof(f))
   seq[i++]=(unsigned char)fgetc(f);
  else 
   seq[i++]=0;
}

int fgetc_wise(FILE*f,int isutf8,unsigned char*seq,int*len)
{
 int ch=0,l=1,state=UTF8_ACCEPT,i;
 seq[0]=fgetc(f); 
 if((seq[0]<128)||(!isutf8))
  {
   ch=seq[0];
   if(len) *len=l;   
  } 
 else 
  {
   if(seq[0]<224)  l=2;
   else 
   if(seq[0]<240)  l=3;
   else            l=4;
   fgetc_read(f,seq+1,l-1); 
   for(i=0;i<l;i++) 
    decode(&state,(unsigned int*)&ch,seq[i]);
   if(len) *len=l;
  }  
 return ch;
}

int read_raw_word(FILE*f,char*element,int elementsize,char*feat,int featsize,int isutf8)
{
 char seq[4];
 int  j=0,l,k,ch;
 *feat=0;
 while(!feof(f))
  {
   ch=fgetc_wise(f,isutf8,seq,&l);
   if((ch==' ')||(ch=='\t')||(ch=='\r')||(ch=='\n'))
    ;
   else
    {
     while(l--)
      ungetc(seq[l],f);
     break; 
    } 
  }
 while(!feof(f))
  {
   ch=fgetc_wise(f,isutf8,seq,&l);
   if(iswpunct(ch))
    {
     if(j==0)
      {
       for(k=0;k<l;k++)
        element[j++]=seq[k];
      } 
     else 
      {
       while(l--)
        ungetc(seq[l],f);
      } 
     break;
    } 
   if((ch==' ')||(ch=='\t')||(ch=='\r')||(ch=='\n'))
    {
     while(l--)
      ungetc(seq[l],f);
     break; 
    } 
   else
    if(j+l<elementsize)
     {
      for(k=0;k<l;k++)
       element[j++]=seq[k];
     } 
  }
 element[j]=0;
 return 1; 
}

int read_conllu_word(FILE*f,char*element,int elementsize,char*feat,int featsize,int isutf8,int which,int conllufilter)
{
 if(!feof(f))
  {
   char line[8192];
   while(!feof(f))
    {
     fgets(line,sizeof(line),f);
     removeendingcrlf(line);      
     if(*line==0)
      continue;
     else
      break; 
    }
   if((*line=='#')||(*line=='<'))
    {
     strcpy(element,line);
     return 1;
    } 
   else
    {
     int         t=0,odd=0;
     char        word[builtin_max_word_len],feature[builtin_max_word_len];
     const char *pline=line;
     
     *word=*feature=0;
     
     while(pline)
      {
       char    piece[builtin_max_word_len];
       pline=gettoken(pline,piece,sizeof(piece)-1,'\t');   
       if(which&512)
        {
         if(t==1)
          strcpy(feature,piece);
         else
         if(t==2)
          strcpy(word,piece);
         else
         if(t==9) 
          {       
           if(strcmp(piece,"#-1")==0)
            *piece=0;       
           if(*piece) 
            {
             strcat(word,piece);
             strcat(word,"\t");
             strcat(word,feature);*feature=0; 
            }
           else
            *word=0;  
           break;
          }
        }
       else 
       if(which&256)
        {
         if(t==2)
          strcpy(word,piece);
         else
         if(t==9) 
          {
           strcpy(feature,piece);
           if((strcmp(feature,"#-1")==0)||(*feature==0))
            *word=*feature=0;
           else
            {
             strcat(feature,"::");
             strcat(feature,word);
             strcpy(word,feature);
             *feature=0;
            } 
           break;
          }
        }
       else 
        if((which&0x7f)==t)
         {
          strcpy(word,piece);
          if(conllufilter==0)
           break;
         } 
        else 
        if(conllufilter)
         if(t==3)
          {
           if((strcmp(piece,"PUNCT")==0)&&(conllufilter&1))
            {*word=*feature=0;break;}
           else
           if((strcmp(piece,"DET")==0)&&(conllufilter&2))
            {*word=*feature=0;break;}
           else
           if((strcmp(piece,"ADP")==0)&&(conllufilter&4))
            {*word=*feature=0;break;}
           else
           if((strcmp(piece,"ADV")==0)&&(conllufilter&8))
            {*word=*feature=0;break;}
           else
           if(((strcmp(piece,"CCONJ")==0)||(strcmp(piece,"SCONJ")==0))&&(conllufilter&16))
            {*word=*feature=0;break;}
           else
           if((strcmp(piece,"AUX")==0)&&(conllufilter&32))
            {*word=*feature=0;break;}
          }
       t++; 
      }     
     strcpy(element,word); 
     strcpy(feat,feature);
     return (*element!=0);
    } 
  }
 else
  return 0; 
}


// --------------------------------------------------------------------

int addcorpus(hquad*hq,int*items,int cnt,int width,int flags,int*perr)
{
 int i,j,err=0,add=0;
 for(i=0;i<cnt;i++)           
  if(items[i]!=-1)  
   {
    for(j=max(0,i-width);j<min(cnt,i+width);j++)
     if(i!=j)
      if(items[j]!=-1)
       if(items[j]!=items[i])
        {
         int addval=1;
         if(flags&1) addval=width-abs(j-i)+1;
         if(hquad_set(hq,items[j],items[i],addval,1)==-1)
          err++;
         else
          add++; 
        }  
   }  
 if(perr) *perr=err;  
 return add;  
}

int filter_word(const char*word,int isutf8,int filter)
{
 if(filter&filter_digits)
  {
   if(isutf8)
    {
     unsigned int state=0,code=0;
     while(*word)
      if(decode(&state,&code,(unsigned char)*word))
       word++;
      else 
       if(iswdigit(code)||(code==',')||(code=='.')||(code=='%')||(code=='\''))
        word++;
       else
        break;    
    }
   else
    while(*word)
     if(isdigit((unsigned char)*word)||((unsigned char)*word==',')||((unsigned char)*word=='.')||((unsigned char)*word=='%')||((unsigned char)*word=='\''))
      word++;
     else
      break;
   if(*word==0)
    return 1;         
  }
 if(filter&filter_punct)
  {
   if(isutf8)
    {
     unsigned int state=0,code=0;
     while(*word)
      if(decode(&state,&code,(unsigned char)*word))
       word++;
      else 
       if(iswpunct(code))
        word++;
       else
        break;        
    }
   else
    while(*word)
     if(ispunct((unsigned char)*word))
      word++;
     else
      break;
   if(*word==0)
    return 1;           
  }  
 return 0;
}

typedef struct{
 int        fileformat;
 int        format;
 
 int        maxdocs;
 int        filter;
 
 int        generating,ngrams;
 
 int        addmode,width;
 
 tfidf_dict*dict;
 tfidf_dict*stop;
 
 hquad     *hq;
}corpus_analysis;

int corpus_analyze(const char*corpus,corpus_analysis*mode)
{
 FILE*f;
 printf("opening %s...\n",corpus);
 f=fopen(corpus,"rb");
 if(f)
  {
   int  docs=0,subdocs=0,llemmas=0;
   int  itemscnt=16*1024,autocut=4*1024;
   int *items=(int*)calloc(itemscnt,sizeof(int));
   int  isutf8=file_checkutf(f),err=0,i=0,add=0;
   setvbuf(f,NULL,_IOFBF,16*1024*1024);
   printf("analyzing...\n",corpus);
   while((!feof(f))&&(!err))
    {
     char word[builtin_max_word_len*2],feat[builtin_max_word_len];
     if(mode->fileformat==fileformat_raw)
      read_raw_word(f,word,sizeof(word),feat,sizeof(feat),isutf8);
     else 
      read_conllu_word(f,word,sizeof(word),feat,sizeof(feat),isutf8,mode->format,mode->filter>>16);     
     if((mode->fileformat==fileformat_conllu)&&((*word=='#')||(*word=='<')))
      {
       if((memcmp(word,"# newdoc",8)==0)||(memcmp(word,"# newpar",8)==0)||(memcmp(word,"<doc",4)==0))
        {
         if(mode->hq)
          add+=addcorpus(mode->hq,items,i,mode->width,mode->addmode,&err);
         i=0;
        }
       if((memcmp(word,"# newdoc",8)==0)||(memcmp(word,"<doc",4)==0))
        {         
         docs++;
         if(mode->hq)
          {
           if((docs%1024)==0)
            printf("doc: %d corpus couples: %dM     \r",docs,mode->hq->used/(1000*1000));            
           if(add>50*1000*1000)
            {
             int red=hquad_reduce(mode->hq,1);
             printf("doc: %d corpus couples: %dM <<  \r",docs,mode->hq->used/(1000*1000));            
             add=0;
            }
          }  
         else
          {
           if((docs%1024)==0)
            printf("doc: %d words: %d     \r",docs,mode->dict->num);          
          } 
         if((mode->maxdocs!=-1)&&(docs>=mode->maxdocs))
          break;
        }
      }
     else 
      {
       if(*word==0)
        items[i++]=-1;
       else 
       if(mode->stop&&tfidf_dict_find(mode->stop,word))
        items[i++]=-1;
       else 
       if(mode->filter&&filter_word(word,isutf8,mode->filter))
        items[i++]=-1;
       else 
        {
         tfidf_lemma*what;
         if(*feat) {strcat(word,"\t");strcat(word,feat);}
         if(mode->generating)
          what=tfidf_dict_add(mode->dict,word,docs+subdocs,1);         
         else
          what=tfidf_dict_find(mode->dict,word);         
         if(what)
          {
           items[i]=(what-mode->dict->items);
           if(mode->ngrams==2)
            {
             if(i&&(items[i-1]!=-1))
              {
               char bigram[512];
               sprintf(bigram,"%s_%s",mode->dict->items[items[i-1]].str,word);
               if(mode->generating)
                tfidf_dict_add(mode->dict,bigram,docs+subdocs,1);   
               else 
                {
                 what=tfidf_dict_find(mode->dict,bigram);         
                 if(what)
                  items[i]=(what-mode->dict->items);
                }
              }
            } 
          } 
         else
          items[i]=-1;
         i++; 
        }  
       if(autocut&&(i>=autocut))
        {
         if(mode->hq) 
          add+=addcorpus(mode->hq,items,i,mode->width,mode->addmode,&err);
         subdocs++;
         i=0;
        }        
      }  
    }    
   printf("\nclosing file.\n");
   fclose(f); 
   free(items);    
   return 1;
  }   
 else
  return 0; 
}

int createneighbors(const char*corpus,const char*dictionary,const char*stops,const char*neighbors,int width,int neighborhoodsize,int filter,int fileformat,int maxdocs,int flags)
{ 
 corpus_analysis crp;
 hquad           hq;
 memset(&crp,0,sizeof(crp)); 
 crp.dict=tfidf_dict_new(256*1024,64*1024,1);
 if(crp.dict)
  {
   printf("reading dictionary file (%s)...\n",dictionary);
   if(tfidf_dict_import(crp.dict,dictionary)==0)
    {
     printf("dictionary file (%s) not found - so it will be generated\n",dictionary);
     crp.generating=1;
     hquad_new(&hq,8192,2*1024*1024,2*1024*1024);  
    }
   else
    {
     crp.generating=0;  
     hquad_new(&hq,8192,crp.dict->num,crp.dict->num);  
    } 
   crp.hq=&hq; 
  }  
 else
  return 0; 
 if(stops&&*stops)
  {
   printf("reading stopwords file (%s)...\n",stops);
   crp.stop=tfidf_dict_new(8192,1024,1);
   if(crp.stop) tfidf_dict_import(crp.stop,stops);
  }   
 crp.ngrams=1+((flags&1)==1);
 crp.fileformat=fileformat&0xFF; 
 crp.format=fileformat>>8;
 crp.filter=filter;
 crp.width=width;
 crp.maxdocs=maxdocs;
 if(corpus_analyze(corpus,&crp))
  {
   int ln=strlen(neighbors),ret;
   printf("optimizing hquad for output...\n");
   hquad_setreadonlymode(crp.hq);     
   printf("\nWriting neighborhoods...\n");     
   if((ln>4)&&(_strcmpi(neighbors+ln-4,".txt")==0))
    ret=hquad_writetext(crp.hq,crp.dict,neighbors,neighborhoodsize);
   else     
    ret=hquad_writebinary(crp.hq,neighbors);     
   if(ret)  
    printf("\ndone.\n");  
   else
    printf("can't write output file\n");     
   if(crp.hq)   hquad_delete(crp.hq);
   if(crp.dict) tfidf_dict_delete(crp.dict);
   if(crp.stop) tfidf_dict_delete(crp.stop);   
   return ret;
  } 
 else
  {
   printf("can't read corpus file.\n");
   return 0;
  }  
}

int createdictionary(const char*corpus,const char*dictionary,const char*stops,int filter,int fileformat,int maxdocs,int flags,int emit,int sortway)
{
 corpus_analysis crp;
 memset(&crp,0,sizeof(crp));
 crp.dict=tfidf_dict_new(256*1024,64*1024,1);
 if(!crp.dict)
  return 0; 
 if(stops&&*stops)
  {
   printf("reading stopwords file (%s)...\n",stops);
   crp.stop=tfidf_dict_new(8192,1024,1);
   if(crp.stop) tfidf_dict_import(crp.stop,stops);
  }     
 crp.fileformat=fileformat&0xFF; 
 crp.format=fileformat>>8; 
 crp.generating=1;
 crp.ngrams=1+((flags&1)==1);
 crp.filter=filter;
 crp.maxdocs=maxdocs;
 if(corpus_analyze(corpus,&crp))
  {
   int hm;
   
   tfidf_dict_settfidf(crp.dict);
    
   if(sortway==0)
    tfidf_dict_sort(crp.dict,tfidf_dict_stringcompare);
   else
    tfidf_dict_sort(crp.dict,tfidf_dict_tfidfcompare); 

   printf("exporting dictionary file (%s)...\n",dictionary);
   hm=tfidf_dict_export(crp.dict,dictionary,emit,2,1);
   printf("Dictionary has %d elements (over cut limits)\n",hm);
   
   if(crp.dict) tfidf_dict_delete(crp.dict);
   if(crp.stop) tfidf_dict_delete(crp.stop);
   
   return hm;
  }
 else
  {
   printf("can't read corpus file.\n");
   return 0;
  }  
}

float row_distance(size_t dsize,int*wordrow,size_t wordrowcnt,int*checkrow,size_t checkrowcnt)
{
 size_t w=0,c=0,cnt=0;
 float  dist=0;
 while((w<wordrowcnt)&&(c<checkrowcnt))
  {
   while((w<wordrowcnt)&&(wordrow[w*2]<checkrow[c*2]))
    {dist+=(wordrow[w*2+1]*wordrow[w*2+1]);w++;cnt++;}
   while((c<checkrowcnt)&&(checkrow[c*2]<wordrow[w*2]))
    {dist+=(checkrow[c*2+1]*checkrow[c*2+1]);c++;cnt++;}    
   while((w<wordrowcnt)&&(c<checkrowcnt)&&(wordrow[w*2]==checkrow[c*2]))
    {dist+=(checkrow[c*2+1]-wordrow[w*2+1])*(checkrow[c*2+1]-wordrow[w*2+1]);w++;c++;cnt++;}
  }
 while(w<wordrowcnt)
  {dist+=(wordrow[w*2+1]*wordrow[w*2+1]);w++;cnt++;} 
 while(c<checkrowcnt)
  {dist+=(checkrow[c*2+1]*checkrow[c*2+1]);c++;cnt++;}     
 return sqrtf(dist); 
}

float row_distanceshare(size_t dsize,int*wordrow,size_t wordrowcnt,int*checkrow,size_t checkrowcnt)
{
 size_t w=0,c=0,cnt=0;
 float  dist=0;
 while((w<wordrowcnt)&&(c<checkrowcnt))
  {
   while((w<wordrowcnt)&&(wordrow[w*2]<checkrow[c*2]))
    w++;
   while((c<checkrowcnt)&&(checkrow[c*2]<wordrow[w*2]))
    c++;
   while((w<wordrowcnt)&&(c<checkrowcnt)&&(wordrow[w*2]==checkrow[c*2]))
    {dist+=(checkrow[c*2+1]*wordrow[w*2+1]);w++;c++;cnt++;}
  }
 if(dist) 
  return sqrtf(dist); 
 else
  return dist; 
}

typedef struct{
 int   id;
 float score;
}best;

void best_reset(best*b,int hm)
{ 
 int i;
 for(i=0;i<hm;i++)
  {b[i].id=-1;b[i].score=0;}
}

void best_add(best*b,int hm,int id,float score,int way)
{
 int i=hm;
 while(i--)
  if(b[i].id==-1)
   if(i&&((b[i-1].id==-1)||((way==-1)&&(b[i-1].score>score))||((way==1)&&(b[i-1].score<score))))
    continue;
   else
    {
     b[i].id=id;
     b[i].score=score;
     break;
    } 
  else
  if(i&&(((way==-1)&&(b[i-1].score>score))||((way==1)&&(b[i-1].score<score))))
   continue;
  else
   {
    int j=hm-1;
    while(j>i)
     {
      b[j].id=b[j-1].id;
      b[j].score=b[j-1].score;
      j--;
     }
    b[i].id=id;
    b[i].score=score;
    break; 
   } 
}

int queryneighbors(const char*dictionary,const char*neighbors,int area)
{ 
 int        ret=0;
 tfidf_dict*dict=tfidf_dict_new(256*1024,64*1024,1);
 if(dict)
  {
   printf("reading dictionary (%s)...\n",dictionary);
   if(tfidf_dict_import(dict,dictionary))
    {
     hquad hq;
     printf("reading neighborhood binary file (%s)...\n",neighbors);
     if(hquad_readbinary(&hq,neighbors))
      {
       int*wordrow=(int*)calloc(area,sizeof(int)*2);
       int*checkrow=(int*)calloc(area,sizeof(int)*2);
       printf("Insert word(s) to get most similar elements (empty to quit):\n");
       while(1)
        {
         char line[1024];
         gets(line);
         removeendingcrlf(line);
         if(*line==0)
          break;
         else
         if(memcmp(line,"show ",5)==0)
          {
           const char   *l=line+5;
           tfidf_lemma  *word[8];
           int          *sum=(int*)calloc(dict->num,sizeof(int));
           unsigned char*mask=(unsigned char*)calloc(dict->num,sizeof(unsigned char));
           size_t        i,j,w=0,pow,wpow=0;
           while(l&&(w<8))
            {
             char wrd[256];
             l=gettoken(l,wrd,sizeof(wrd),' ');
             word[w]=tfidf_dict_find(dict,wrd);
             if(word[w]==NULL)
              printf("word \"%s\" not in dictionary, sorry.\n",wrd);
             else
              w++; 
            }
           for(pow=1,i=0;i<w;i++,pow*=2)
            {
             size_t y,id=word[i]-dict->items;
             size_t wordrowcnt=hquad_getreadonlyrow(&hq,id,wordrow,area);
             for(y=0;y<wordrowcnt;y++)
              if(wordrow[y*2+1]>1)
               {
                sum[wordrow[y*2]]+=wordrow[y*2+1];
                mask[wordrow[y*2]]|=pow;
               }
             wpow|=pow;  
            }
           for(j=i=0;i<dict->num;i++)
            if(sum[i]&&(mask[i]==wpow))
             {wordrow[j*2]=i;wordrow[j*2+1]=sum[i];j++;}
           qsort(wordrow,j,sizeof(int)*2,cnt_compare);  
           for(i=0;i<j;i++) 
            {
             if(i) printf(", ");
             printf("%s",dict->items[wordrow[i*2]].str);
            }
           printf("\n");             
           free(sum); 
          }
         else
          {
           tfidf_lemma*word=tfidf_dict_find(dict,line);
           if(word==NULL)
            printf("word not in dictionary, sorry.\n");
           else
            {
             size_t y,id=word-dict->items;
             best   b[16];
             int    bestid=-1;
             float  distance,bestdistance=-1;
             size_t wordrowcnt=hquad_getreadonlyrow(&hq,id,wordrow,area);
             qsort(wordrow,wordrowcnt,sizeof(int)*2,id_compare);
             best_reset(&b[0],sizeof(b)/sizeof(b[0]));
             for(y=0;y<dict->num;y++)     
              if(y!=id)
               {
                size_t checkrowcnt=hquad_getreadonlyrow(&hq,y,checkrow,area);          
                if(checkrowcnt)
                 {     
                  qsort(checkrow,checkrowcnt,sizeof(int)*2,id_compare);
                  distance=row_distanceshare(dict->num,wordrow,wordrowcnt,checkrow,checkrowcnt);
                  //best_add(&b[0],sizeof(b)/sizeof(b[0]),y,distance,-1);
                  best_add(&b[0],sizeof(b)/sizeof(b[0]),y,distance,1);
                 }  
               }            
             printf("Similar to: ");
             for(y=0;y<sizeof(b)/sizeof(b[0]);y++)
              if(b[y].id!=-1)
               { 
                if(y) printf(", ");               
                printf("%s",dict->items[b[y].id].str);  
               } 
              else
               break; 
             printf("\n");  
            } 
          } 
        }
       free(checkrow);
       free(wordrow); 
      }
     else
      {printf("can't read neighborhood (binary) file\n");ret=0;}
    }   
   else 
    {printf("can't read dictionary file\n");ret=0;}
  }  
 else   
  {printf("can't create dictionary\n");ret=0;}
 return ret;
}

// --------------------------------------------------------------------

int main(int argc,char* argv[])
{
 if(argc==1)
  {
   printf("Word2Neighborhood\n");
   printf(" A simple tool to create dictionary or context data from a corpus file");
   printf(" (raw or in CoNLLU format), ansi or utf8, using TFxIDF\n");
   printf("\n");
   printf("Options:\n");
   printf(" -corpus/-crp <filename> [needed, corpus to analyze]\n");
   printf("[build]\n");
   printf(" -create/-c dictionary|dict|d / neighborhood|neighbors|n [needed]\n");
   printf("  create a dictionary from corpus or create neighborhood from dictionary&corpus\n");      
   printf(" -corpusformat/-crpf conllu-lemma / conllu-form [normal text if not specified]\n");         
   printf(" -dict <filename> [<corpus>.lex if not specified]\n");
   printf(" -stopwords/-s <filename>\n");
   printf(" -neighbors/-n <filename> [neighborhood output file, <corpus.neighbors> if not specified]\n");   
   printf(" -maxdocs <doc number> [max document number to read form corpus file]\n");
   printf(" -sort 0 = alphabetic 1 = tdidf [sort order when creating dictionary]\n");
   printf(" -emit 1 = wordcnt | 2 = doccnt | 4 = tfidf [extra data to store in dictionary file]\n");
   printf(" -filter 1 = digits | 2 = punct [filter, used with stopword file, to skip words]\n");
   printf(" -width <width size> [radius used when creating neighborhood data, default 16]\n");
   printf(" -area <area size> [neighborhood max size for output, default: 64]\n");
   printf(" -bigrams [consider/generate bigrams]\n");
   printf("[query]\n");
   printf(" -query [consider/generate bigrams]\n\n");
   printf("Examples:\n");
   printf("[build dictionary from a corpus file]\n");
   printf(" word2neigh -c dictionary -crp \"war&peace.txt\" -dict novel.txt -stop en.stopwords.txt\n");
   printf(" word2neigh -c dictionary -crp wiki.collnu -crpf conllu-lemma -dict wikidict.txt\n\n");
   printf("[build readable neighborhood file from a corpus file]\n");
   printf(" word2neigh -c neighborhood -crp wiki.collnu -crpf conllu-lemma -dict wikidict.txt -neighbors matrix.txt\n\n");
   printf("[build dictionary + binary neighborhood file from a corpus file]\n");
   printf(" word2neigh -c d -crp \"war&peace.txt\" -stop en.stopwords.txt\n");
   printf(" word2neigh -c n -crp \"war&peace.txt\"\n\n");
   printf("[query from dictionary + binary neighborhood files]\n");
   printf(" word2neigh -q -crp \"war&peace.txt\"\n");
   return 0;
  }
 else
  {
   char value[256],corpus[256],dict[256],stopwords[256],neighbors[256];
   int  mode=0,fileformat=fileformat_raw,format=2,maxdocs=-1,width=16,area=64,flags=0,sortway=1,filter=filter_punct|filter_digits,conllufilter=1|2|4|8|16|32,emit=1|2|4;
   *corpus=*dict=*stopwords=*neighbors=00;
   if(getparam("-create",argc,argv,value)||getparam("-c",argc,argv,value))
    {
     if((strcmp(value,"dict")==0)||(strcmp(value,"dictionary")==0)||(strcmp(value,"d")==0))
      mode=1;
     else
     if((strcmp(value,"neighbors")==0)||(strcmp(value,"neighborhood")==0)||(strcmp(value,"n")==0))
      mode=2;      
    }
   else 
   if(getparam("-query",argc,argv,value)||getparam("-q",argc,argv,value))
    mode=3;
   else
    printf("missing -create param (dictionary or neighborhood request)\n");
   if(getparam("-corpus",argc,argv,value)||getparam("-crp",argc,argv,value)) 
    strcpy(corpus,value);
   else
   if(mode!=3)
    printf("missing -corpus param (corpus file name)\n");
   if(getparam("-corpusformat",argc,argv,value)||getparam("-crpf",argc,argv,value)) 
    {
     if((strcmp(value,"conllu-lemma")==0)||(strcmp(value,"lemma")==0))
      {format=2;fileformat=fileformat_conllu;}
     else
     if((strcmp(value,"conllu-form")==0)||(strcmp(value,"form")==0))
      {format=1;fileformat=fileformat_conllu;}
     else
     if((strcmp(value,"raw")==0)||(strcmp(value,"line")==0))
      format=0;       
     else
     if(strcmp(value,"form+lemma")==0)
      {format=512;fileformat=fileformat_conllu;}
     else
     if(strcmp(value,"sem")==0)
      {format=256;fileformat=fileformat_conllu;}
    }     
   if(getparam("-dict",argc,argv,value)||getparam("-d",argc,argv,value)) 
    strcpy(dict,value);    
   else
    {
     if(*corpus)
      {strcpy(dict,corpus);setextension(dict,"lex");}
     else
      strcpy(dict,"dictionary.txt");
    } 
   if(getparam("-neighbors",argc,argv,value)||getparam("-n",argc,argv,value)) 
    strcpy(neighbors,value);    
   else
     if(*corpus)
      {strcpy(neighbors,corpus);setextension(neighbors,"neighbors");}
     else   
     strcpy(neighbors,"neighbors.txt");    
   if(getparam("-stopwords",argc,argv,value)||getparam("-s",argc,argv,value)) 
    strcpy(stopwords,value);  
   if(getparam("-maxdocs",argc,argv,value))
    maxdocs=atoi(value);
   if(getparam("-width",argc,argv,value))
    width=atoi(value); 
   if(getparam("-area",argc,argv,value))
    area=atoi(value);     
   if(getparam("-sort",argc,argv,value))
    sortway=atoi(value);      
   if(getparam("-filter",argc,argv,value))
    filter=atoi(value);       
   if((getparam("-conllufilter",argc,argv,value))||(getparam("-cf",argc,argv,value)))
    conllufilter=atoi(value);        
   if(getparam("-emit",argc,argv,value))
    emit=atoi(value);        
   if(getparam("-bigrams",argc,argv,value))
    flags|=1;           
   
   printf("Word2Neighborhood\n");
   switch(mode)
    {
     case 1:
      createdictionary(corpus,dict,stopwords,filter|(conllufilter<<16),fileformat|(format<<8),maxdocs,flags,emit,sortway);
     break;
     case 2:
      createneighbors(corpus,dict,stopwords,neighbors,width,area,filter|(conllufilter<<16),fileformat|(format<<8),maxdocs,flags);
     break;
     case 3:
      queryneighbors(dict,neighbors,area);
     break;
    }       
  }   
 return 1;
}