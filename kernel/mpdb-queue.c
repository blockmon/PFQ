/***************************************************************
 *                                                
 * (C) 2011-12 Nicola Bonelli <nicola.bonelli@cnit.it>   
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <mpdb-queue.h>

void *
mpdb_queue_alloc(struct pfq_opt *pq, size_t queue_mem, size_t * tot_mem)
{
        /* calculate the size of the buffer */

        size_t tm = PAGE_ALIGN(queue_mem); 

        /* align bufflen to page size */

        size_t num_pages = tm / PAGE_SIZE; void *addr;

        num_pages += (num_pages + (SHMLBA-1)) % SHMLBA;
        *tot_mem = num_pages*PAGE_SIZE;

        /* Memory is already zeroed */
        addr = vmalloc_user(*tot_mem);
        if (addr == NULL)
        {
                printk(KERN_INFO "[PF_Q] pfq_queue_alloc: out of memory");
                *tot_mem = 0;
                return NULL;
        }

        printk(KERN_INFO "[PF_Q] queue caplen:%lu mem:%lu\n", pq->q_caplen, *tot_mem); 
        return addr;
}


void
mpdb_queue_free(struct pfq_opt *pq)
{
        if (pq->q_addr) {
                printk(KERN_INFO "[PF_Q] queue freed!\n"); 
                vfree(pq->q_addr);

                pq->q_addr = NULL;
                pq->q_queue_mem = 0;
        }
}    


bool 
mpdb_enqueue(struct pfq_opt *pq, struct sk_buff *skb)
{
        struct pfq_queue_descr  *queue_descr = (struct pfq_queue_descr *)pq->q_addr;

        if (!atomic_read((atomic_t *)&queue_descr->disabled))  
        {
                size_t packet_len = skb->len + skb->mac_len;
                size_t bytes = (packet_len > pq->q_offset) ? min(packet_len - pq->q_offset, pq->q_caplen) : 0;

                int  data    = atomic_add_return(1, (atomic_t *)&queue_descr->data);
                int  q_len   = DBMP_QUEUE_LEN(data);
                bool q_index = DBMP_QUEUE_INDEX(data);

                if (q_len <= pq->q_slots)
                {
                        /* enqueue skb */

                        struct pfq_hdr *p_hdr = (struct pfq_hdr *)((char *)(queue_descr+1) + q_index * pq->q_slot_size * pq->q_slots 
                                        + (q_len-1) * pq->q_slot_size);

                        char *p_pkt = (char *)(p_hdr+1);

                        /* copy bytes of packet */

                        if (bytes && skb_copy_bits(skb, pq->q_offset - skb->mac_len, p_pkt, bytes) != 0)
                        {    
                                return false;
                        }

                        /* setup the header */

                        p_hdr->len      = packet_len;
                        p_hdr->caplen   = bytes;
                        p_hdr->if_index = skb->dev->ifindex;
                        p_hdr->hw_queue = skb_get_rx_queue(skb);                      

                        if (pq->q_tstamp != 0)
                        {
                                struct timespec ts;
                                skb_get_timestampns(skb, &ts); 
                                p_hdr->tstamp.tv.sec  = ts.tv_sec;
                                p_hdr->tstamp.tv.nsec = ts.tv_nsec;
                        }

                        /* commit the slot with release semantic */
                        smp_wmb();

                        p_hdr->commit = 1;

                        /* watermark */

                        if ((q_len > ( pq->q_slots >> 1)) 
                                        && queue_descr->poll_wait ) {
                                wake_up_interruptible(&pq->q_waitqueue);
                        }

                        return true;
                }
                else if (q_len == (pq->q_slots+1))
                {
                        atomic_set((atomic_t *)&queue_descr->disabled,1);
                }
        }

        if ( queue_descr->poll_wait ) {
                wake_up_interruptible(&pq->q_waitqueue);
        }

        return false;
}

