/*
  eXosip - This is the eXtended osip library.
  Copyright (C) 2002, 2003  Aymeric MOIZARD  - jack@atosc.org
  
  eXosip is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  eXosip is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#ifdef ENABLE_MPATROL
#include <mpatrol.h>
#endif

#include <eXosip.h>
#include <eXosip2.h>

#include <osip2/thread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
//#include <arpa/inet.h>

#include <unistd.h>

eXosip_t eXosip;
char   *localip;
char   *localport;


int
eXosip_lock()
{
  return smutex_lock((struct smutex_t*)eXosip.j_mutexlock);
}

int
eXosip_unlock()
{
  return smutex_unlock((struct smutex_t*)eXosip.j_mutexlock);
}

void
eXosip_kill_transaction (list_t * transactions)
{
  transaction_t *transaction;

  if (!list_eol (transactions, 0))
    {
      /* some transaction are still used by osip,
         transaction should be released by modules! */
      OSIP_TRACE (osip_trace
		  (__FILE__, __LINE__, OSIP_ERROR, NULL,
		   "module sfp: _osip_kill_transaction transaction should be released by modules!\n"));
    }

  while (!list_eol (transactions, 0))
    {
      transaction = list_get (transactions, 0);
      transaction_free (transaction);
      sfree (transaction);
    }
}

void eXosip_quit()
{
  eXosip_call_t *jc;
  eXosip_reg_t  *jreg;
  int i;

  eXosip.j_stop_ua = 1; /* ask to quit the application */
  i = sthread_join((struct sthread_t*)eXosip.j_thread);
  if (i!=0)
    fprintf(stderr, "eXosip: can't terminate thread!");
  sfree((struct sthread_t*)eXosip.j_thread);

  eXosip.j_input = 0;
  eXosip.j_output = 0;

  for (jc = eXosip.j_calls; jc!=NULL;jc = eXosip.j_calls)
    {
      REMOVE_ELEMENT(eXosip.j_calls, jc);
      eXosip_call_free(jc);
    }
  
  smutex_destroy((struct smutex_t*)eXosip.j_mutexlock);

  sdp_config_free();  

  if (eXosip.j_input)
    fclose(eXosip.j_input);
  if (eXosip.j_output)
    sfree(eXosip.j_output);
  if (eXosip.j_socket)
    close(eXosip.j_socket);

  for (jreg = eXosip.j_reg; jreg!=NULL; jreg = eXosip.j_reg)
    {
      REMOVE_ELEMENT(eXosip.j_reg, jreg);
      eXosip_reg_free(jreg);
    }

  /* should be moved to method with an argument */
  jfreind_unload();
  jidentity_unload();

  /*    
  for (jid = eXosip.j_identitys; jid!=NULL; jid = eXosip.j_identitys)
    {
      REMOVE_ELEMENT(eXosip.j_identitys, jid);
      eXosip_freind_free(jid);
    }

  for (jfr = eXosip.j_freinds; jfr!=NULL; jfr = eXosip.j_freinds)
    {
      REMOVE_ELEMENT(eXosip.j_freinds, jfr);
      eXosip_reg_free(jfr);
    }
  */

  while (!list_eol(eXosip.j_transactions, 0))
    {
      transaction_t *tr = (transaction_t*) list_get(eXosip.j_transactions, 0);
      if (tr->state==IST_TERMINATED || tr->state==ICT_TERMINATED
	  || tr->state== NICT_TERMINATED || tr->state==NIST_TERMINATED)
	{
	  OSIP_TRACE(osip_trace(__FILE__,__LINE__,OSIP_INFO1,NULL,
		      "Release a terminated transaction\n"));
	  list_remove(eXosip.j_transactions, 0);
	  transaction_free2(tr);
	}
      else
	{
	  list_remove(eXosip.j_transactions, 0);
	  transaction_free(tr);
	}
    }

  eXosip_kill_transaction (eXosip.j_osip->ict_transactions);
  eXosip_kill_transaction (eXosip.j_osip->nict_transactions);
  eXosip_kill_transaction (eXosip.j_osip->ist_transactions);
  eXosip_kill_transaction (eXosip.j_osip->nist_transactions);
  osip_free (eXosip.j_osip);
  osip_global_free ();

  return ;
}

int eXosip_execute ( void )
{
  int i;
  eXosip_lock();
  i = eXosip_read_message(1, 0, 200000);
  eXosip_unlock();
  if (i==-2)
    return -2;
  
  osip_timers_ict_execute(eXosip.j_osip);
  osip_timers_nict_execute(eXosip.j_osip);
  osip_timers_ist_execute(eXosip.j_osip);
  osip_timers_nist_execute(eXosip.j_osip);
  
  eXosip_lock();

  osip_ict_execute(eXosip.j_osip);
  osip_nict_execute(eXosip.j_osip);
  osip_ist_execute(eXosip.j_osip);
  osip_nist_execute(eXosip.j_osip);
  
  // free all Calls that are in the TERMINATED STATE? */
  eXosip_free_terminated_calls();

  eXosip_unlock();

  return 0;
}

void *eXosip_thread        ( void *arg )
{
  int i;
  while (eXosip.j_stop_ua==0)
    {
      i = eXosip_execute();
      if (i==-2)
	sthread_exit();
    }
  sthread_exit();
  return NULL;
}

int eXosip_init(FILE *input, FILE *output, int port)
{
  osip_t *osip;
  int i;
  if (port<0)
    {
      fprintf(stderr, "eXosip: port must be higher than 0!\n");
      return -1;
    }
  eXosip_guess_ip_for_via(&localip);
  if (localip==NULL)
    {
#ifdef ENABLE_DEBUG
      fprintf(stderr, "eXosip: No ethernet interface found!\n");
      fprintf(stderr, "eXosip: using 127.0.0.1 (debug mode)!\n");
      localip = sgetcopy("127.0.0.1");
#else
      fprintf(stderr, "eXosip: No ethernet interface found!\n");
      return -1;
#endif
    }

  eXosip.j_input = input;
  eXosip.j_output = output;
  eXosip.j_calls = NULL;
  eXosip.j_stop_ua = 0;
  eXosip.j_thread = NULL;
  eXosip.j_transactions = (list_t*) smalloc(sizeof(list_t));
  list_init(eXosip.j_transactions);
  eXosip.j_reg = NULL;

  eXosip.j_mutexlock = (struct smutex_t*)smutex_init();

  if (-1==osip_init(&osip))
    {
      fprintf(stderr, "eXosip: Cannot initialize osip!\n");
      return -1;
    }
  if (-1==osip_global_init())
    {
      fprintf(stderr, "eXosip: Cannot initialize osip!\n");
      return -1;
    }

  eXosip_sdp_config_init();

  osip_set_application_context(osip, &eXosip);
  
  eXosip_set_callbacks(osip);
  
  eXosip.j_osip = osip;

  // open the UDP listener
          
  eXosip.j_socket = (int)socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (eXosip.j_socket==-1)
    return -1;
  
  {
    struct sockaddr_in  raddr;
    raddr.sin_addr.s_addr = htons(INADDR_ANY);
    raddr.sin_port = htons((short)port);
    raddr.sin_family = AF_INET;
    
    i = bind(eXosip.j_socket, (struct sockaddr *)&raddr, sizeof(raddr));
    if (i < 0)
    {
      fprintf(stderr, "eXosip: Cannot bind on port: %i!\n", i);
      return -1;
    }
  }

  localport = (char*)smalloc(10);
  sprintf(localport, "%i", port);

  eXosip.j_thread = (void*) sthread_create(20000,eXosip_thread, NULL);
  if (eXosip.j_thread==NULL)
    {
      fprintf(stderr, "eXosip: Cannot start thread!\n");
      return -1;
    }

  return 0;
}

void
eXosip_update()
{
  eXosip_call_t *jc;
  eXosip_dialog_t *jd;
  int counter=1;
  int counter2=1;
  for (jc=eXosip.j_calls; jc!=NULL; jc=jc->next)
    {
      jc->c_id = counter2;
      counter2++;
      for (jd=jc->c_dialogs; jd!=NULL; jd=jd->next)
	{
	  if (jd->d_dialog!=NULL) /* finished call */
	    {
	      jd->d_id = counter;
	      counter++;
	    }
	  else jd->d_id = -1;
	}
    }
}

void eXosip_message    (char *to, char *from, char *route, char *buff)
{
  /* eXosip_call_t *jc;
     header_t *subject; */
  sip_t *message;
  transaction_t *transaction;
  sipevent_t *sipevent;
  int i;

  i = generating_message(&message, to, from, route, buff);
  if (i!=0) 
    {
      fprintf(stderr, "eXosip: cannot send message (cannot build MESSAGE)! ");
      return;
    }

  i = transaction_init(&transaction,
		       NICT,
		       eXosip.j_osip,
		       message);
  if (i!=0)
    {
      /* TODO: release the j_call.. */

      msg_free(message);
      return ;
    }
  
  list_add(eXosip.j_transactions, transaction, 0);
  
  sipevent = osip_new_outgoing_sipmessage(message);
  sipevent->transactionid =  transaction->transactionid;
  
  transaction_add_event(transaction, sipevent);

  //  ADD_ELEMENT(eXosip.j_calls, jc);
  transaction_set_your_instance(transaction, new_jinfo(NULL, NULL));
}

void eXosip_start_call    (sip_t *invite)
{
  //  static int static_jcid = 0; /* This value is used as a unique id for call */

  eXosip_call_t *jc;
  header_t *subject;
  transaction_t *transaction;
  sipevent_t *sipevent;
  int i;
  sdp_t *sdp;
  char *body;
  char *size;
  
  sdp_build_offer(NULL, &sdp, "10500", NULL);

  i = sdp_2char(sdp, &body);
  if (body!=NULL)
    {
      size= (char *)smalloc(7*sizeof(char));
      sprintf(size,"%i",strlen(body));
      msg_setcontent_length(invite, size);
      sfree(size);
      
      msg_setbody(invite, body);
      sfree(body);
      msg_setcontent_type(invite, "application/sdp");
    }
  else
    msg_setcontent_length(invite, "0");

  jc = (eXosip_call_t *) smalloc(sizeof(eXosip_call_t));
  i = msg_getsubject(invite, 0, &subject);
  snprintf(jc->c_subject, 99, "%s", subject->hvalue);
  jc->c_dialogs = NULL;
  
  sdp_context_init(&(jc->c_ctx));
  sdp_context_set_mycontext(jc->c_ctx, jc);
  sdp_context_set_local_sdp(jc->c_ctx, sdp);  

  jc->c_inc_tr = NULL;
  jc->c_out_tr = NULL;

  jc->next     = NULL;
  jc->parent   = NULL;

  i = transaction_init(&transaction,
		       ICT,
		       eXosip.j_osip,
		       invite);
  if (i!=0)
    {
      /* TODO: release the j_call.. */

      msg_free(invite);
      return ;
    }
  
  jc->c_out_tr = transaction;
  
  sipevent = osip_new_outgoing_sipmessage(invite);
  sipevent->transactionid =  transaction->transactionid;
  
  transaction_add_event(transaction, sipevent);

  ADD_ELEMENT(eXosip.j_calls, jc);
  transaction_set_your_instance(transaction, new_jinfo(jc, NULL));
}

void eXosip_answer_call   (int jid, int status)
{
  eXosip_dialog_t *jd = NULL;
  eXosip_call_t *jc = NULL;
  if (jid>0)
    {
      eXosip_dialog_find(jid, &jc, &jd);
    }
  if (jd==NULL)
    {
      fprintf(stderr, "eXosip: No call here?\n");
      return;
    }
  if (status>100 && status<200)
    {
      eXosip_answer_invite_1xx(jc, jd, status);
    }
  else if (status>199 && status<300)
    {
      eXosip_answer_invite_2xx(jc, jd, status);
    }
  else if (status>300 && status<699)
    {
      eXosip_answer_invite_3456xx(jc, jd, status);
    }
  else
    {
      fprintf(stderr, "eXosip: wrong status code (101<status<699)\n");
      return;
    }
}

void eXosip_on_hold_call  (int jid)
{
  eXosip_dialog_t *jd = NULL;
  eXosip_call_t *jc = NULL;

  transaction_t *transaction;
  sipevent_t *sipevent;
  sip_t *invite;
  int i;
  sdp_t *sdp;
  char *body;
  char *size;

  if (jid>0)
    {
      eXosip_dialog_find(jid, &jc, &jd);
    }
  if (jd==NULL)
    {
      fprintf(stderr, "eXosip: No call here?\n");
      return;
    }

  transaction = eXosip_find_last_invite(jc, jd);
  if (transaction==NULL) return;
  if (transaction->state!=ICT_TERMINATED &&
      transaction->state!=IST_TERMINATED)
    return;

  sdp = eXosip_get_local_sdp_info(transaction);
  if (sdp==NULL)
    return;
  i = sdp_put_on_hold(sdp);
  if (i!=0)
    {
      sdp_free(sdp);
      return ;
    }

  i = _eXosip_build_request_within_dialog(&invite, "INVITE", jd->d_dialog, "UDP");
  if (i!=0) {
    sdp_free(sdp);
    return;
  }

  i = sdp_2char(sdp, &body);
  if (body!=NULL)
    {
      size= (char *)smalloc(7*sizeof(char));
      sprintf(size,"%i",strlen(body));
      msg_setcontent_length(invite, size);
      sfree(size);
      
      msg_setbody(invite, body);
      sfree(body);
      msg_setcontent_type(invite, "application/sdp");
    }
  else
    msg_setcontent_length(invite, "0");

  if (jc->c_subject!=NULL)
    msg_setsubject(invite, jc->c_subject);
  else
    msg_setsubject(invite, jc->c_subject);

  i = transaction_init(&transaction,
		       ICT,
		       eXosip.j_osip,
		       invite);
  if (i!=0)
    {
      /* TODO: release the j_call.. */
      msg_free(invite);
      return ;
    }
  
  
  {
    sdp_t *old_sdp = sdp_context_get_local_sdp(jc->c_ctx);
    sdp_free(old_sdp);
    sdp_context_set_local_sdp(jc->c_ctx, sdp);  
  }

  list_add(jd->d_out_trs, transaction, 0);
  
  sipevent = osip_new_outgoing_sipmessage(invite);
  sipevent->transactionid =  transaction->transactionid;
  
  transaction_add_event(transaction, sipevent);

  transaction_set_your_instance(transaction, new_jinfo(jc, NULL));  
}

void eXosip_off_hold_call (int jid)
{

}

int eXosip_create_transaction(eXosip_call_t *jc,
			     eXosip_dialog_t *jd,
			     sip_t *request)
{
  sipevent_t *sipevent;
  transaction_t *tr;
  int i;
  i = transaction_init(&tr,
		       NICT,
		       eXosip.j_osip,
		       request);
  if (i!=0)
    {
      /* TODO: release the j_call.. */

      msg_free(request);
      return -1;
    }
  
  if (jd!=NULL)
    list_add(jd->d_out_trs, tr, 0);
  
  sipevent = osip_new_outgoing_sipmessage(request);
  sipevent->transactionid =  tr->transactionid;
  
  transaction_add_event(tr, sipevent);
  transaction_set_your_instance(tr, new_jinfo(jc, jd));
  return 0;
}

void eXosip_transfer_call(int jid, char *refer_to)
{
  int i;
  sip_t *request;
  eXosip_dialog_t *jd = NULL;
  eXosip_call_t *jc = NULL;
  if (jid<=0)
    return;

  eXosip_dialog_find(jid, &jc, &jd);
  if (jd==NULL || jd->d_dialog==NULL || jd->d_dialog->state==DIALOG_EARLY)
    {
      fprintf(stderr, "eXosip: No established call here!");
      return;
    }

  i = generating_refer(&request, jd->d_dialog, refer_to);
  if (i!=0)
    fprintf(stderr, "eXosip: cannot terminate this call! ");

  i = eXosip_create_transaction(jc, jd, request);
  if (i!=0)
    fprintf(stderr, "eXosip: cannot initiate SIP transfer transaction!");
}

int eXosip_create_cancel_transaction(eXosip_call_t *jc,
				    eXosip_dialog_t *jd, sip_t *request)
{
  sipevent_t *sipevent;
  transaction_t *tr;
  int i;
  i = transaction_init(&tr,
		       NICT,
		       eXosip.j_osip,
		       request);
  if (i!=0)
    {
      /* TODO: release the j_call.. */

      msg_free(request);
      return -1;
    }
  
  list_add(eXosip.j_transactions, tr, 0);
  
  sipevent = osip_new_outgoing_sipmessage(request);
  sipevent->transactionid =  tr->transactionid;
  
  transaction_add_event(tr, sipevent);
  return 0;
}

void eXosip_terminate_call(int cid, int jid)
{
  int i;
  sip_t *request;
  eXosip_dialog_t *jd = NULL;
  eXosip_call_t *jc = NULL;
  if (jid>0)
    {
      eXosip_dialog_find(jid, &jc, &jd);
      if (jd==NULL)
	{
	  fprintf(stderr, "eXosip: No call here? ");
	  return;
	}
    }
  else
    {
      eXosip_call_find(cid, &jc);      
    }

  if (jd==NULL || jd->d_dialog==NULL)
    {
      transaction_t *tr;
      fprintf(stderr, "eXosip: No established dialog!");
      //#warning TODO: choose the latest not the first one.
      tr=jc->c_out_tr;
      if (tr!=NULL && tr->last_response!=NULL && MSG_IS_STATUS_1XX(tr->last_response))
	{
	  i = generating_cancel(&request, tr->orig_request);
	  if (i!=0)
	    fprintf(stderr, "eXosip: cannot terminate this call! ");
	  return;
	  i = eXosip_create_cancel_transaction(jc, jd, request);
	  if (i!=0)
	    fprintf(stderr, "eXosip: cannot initiate SIP transaction! ");
	}
      return;
    }
  i = generating_bye(&request, jd->d_dialog);
  if (i!=0)
    fprintf(stderr, "eXosip: cannot terminate this call! ");

  i = eXosip_create_transaction(jc, jd, request);
  if (i!=0)
    fprintf(stderr, "eXosip: cannot initiate SIP transaction! ");

  dialog_free(jd->d_dialog);
  jd->d_dialog = NULL;
}

void eXosip_register      (int rid)
{
  transaction_t *transaction;
  sipevent_t *sipevent;
  sip_t *reg;
  int i;

  eXosip_reg_t *jr;

  jr = eXosip.j_reg;
  if (jr==NULL)
    {
      fprintf(stderr, "eXosip: no registration info saved!\n");
      return ;
    }
  reg = NULL;
  if (jr->r_last_tr!=NULL)
    {
      if (jr->r_last_tr->state!=NICT_TERMINATED)
	{
	  fprintf(stderr, "eXosip: a registration is already pending!\n");
	  return ;
	}
      else
	{
	  reg = jr->r_last_tr->orig_request;
	  jr->r_last_tr->orig_request = NULL;
	  transaction_free2(jr->r_last_tr);
	  jr->r_last_tr = NULL;

	  /* modify the REGISTER request */
	  {
	    int cseq_num = satoi(reg->cseq->number);
	    int length   = strlen(reg->cseq->number);
	    char *tmp    = (char *)smalloc(90*sizeof(char));
	    via_t *via   = (via_t *) list_get (reg->vias, 0);
	    list_remove(reg->vias, 0);
	    via_free(via);
	    sprintf(tmp, "SIP/2.0/UDP %s:%s;branch=z9hG4bK%u",
		    localip,
		    localport,
		    via_branch_new_random());
	    via_init(&via);
	    via_parse(via, tmp);
	    list_add(reg->vias, via, 0);
	    sfree(tmp);

	    cseq_num++;
	    sfree(reg->cseq->number);
	    reg->cseq->number = (char*)smalloc(length+2); /* +2 like for 9 to 10 */
	    sprintf(reg->cseq->number, "%i", cseq_num);
	    
	  }
	}
    }
  if (reg==NULL)
    {
      i = generating_register(&reg, jr->r_aor, jr->r_registrar, jr->r_contact);
      if (i!=0) 
	{
	  fprintf(stderr, "eXosip: cannot register (cannot build REGISTER)! ");
	  return;
	}
    }

  i = transaction_init(&transaction,
		       NICT,
		       eXosip.j_osip,
		       reg);
  if (i!=0)
    {
      /* TODO: release the j_call.. */

      msg_free(reg);
      return ;
    }

  jr->r_last_tr = transaction;

  /* send REGISTER */
  sipevent = osip_new_outgoing_sipmessage(reg);
  sipevent->transactionid =  transaction->transactionid;
  msg_force_update(reg);
  
  transaction_add_event(transaction, sipevent);

}


void
eXosip_register_init(char *from, char *proxy, char *contact)
{
  eXosip_reg_t *jr;
  int i;

  i = eXosip_reg_init(&jr, from, proxy, contact);
  if (i!=0) 
    {
      fprintf(stderr, "eXosip: cannot register! ");
      return ;
    }
  eXosip.j_reg = jr;
}
