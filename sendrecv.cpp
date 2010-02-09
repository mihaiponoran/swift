/*
 *  sendrecv.cpp
 *  most of the swift's state machine
 *
 *  Created by Victor Grishchenko on 3/6/09.
 *  Copyright 2009 Delft University of Technology. All rights reserved.
 *
 */
#include "swift.h"
#include "compat/util.h"


using namespace swift;
using namespace std;

/*
 TODO  25 Oct 18:55
 - range: ALL
 - randomized testing of advanced ops (new testcase)
 */

void    Channel::AddPeakHashes (Datagram& dgram) {
    for(int i=0; i<file().peak_count(); i++) {
        bin64_t peak = file().peak(i);
        dgram.Push8(SWIFT_HASH);
        dgram.Push32((uint32_t)peak);
        dgram.PushHash(file().peak_hash(i));
        //DLOG(INFO)<<"#"<<id<<" +pHASH"<<file().peak(i);
        dprintf("%s #%u +phash %s\n",tintstr(),id_,peak.str());
    }
}


void    Channel::AddUncleHashes (Datagram& dgram, bin64_t pos) {
    bin64_t peak = file().peak_for(pos);
    while (pos!=peak && ((NOW&3)==3 || !data_out_cap_.within(pos.parent())) &&
            ack_in_.get(pos.parent())==binmap_t::EMPTY  ) {
        bin64_t uncle = pos.sibling();
        dgram.Push8(SWIFT_HASH);
        dgram.Push32((uint32_t)uncle);
        dgram.PushHash( file().hash(uncle) );
        //DLOG(INFO)<<"#"<<id<<" +uHASH"<<uncle;
        dprintf("%s #%u +hash %s\n",tintstr(),id_,uncle.str());
        pos = pos.parent();
    }
}


bin64_t        Channel::DequeueHint () {
    if (hint_in_.empty() && last_recv_time_>NOW-rtt_avg_-TINT_SEC) {    
        uint64_t twist = peer_channel_id_;  // got no hints, send something randomly
        twist &= file().peak(0); // may make it semi-seq here
        file().ack_out().twist(twist);
        ack_in_.twist(twist);
        bin64_t my_pick = 
            file().ack_out().find_filtered(ack_in_,bin64_t::ALL,binmap_t::FILLED);
        while (my_pick.width()>max(1,(int)cwnd_))
            my_pick = my_pick.left();
        file().ack_out().twist(0);
        ack_in_.twist(0);
        if (my_pick!=bin64_t::NONE) {
            my_pick = my_pick.twisted(twist);
            hint_in_.push_back(my_pick);
            dprintf("%s #%u *hint %s\n",tintstr(),id_,my_pick.str());
        }
    }
    bin64_t send = bin64_t::NONE;
    while (!hint_in_.empty() && send==bin64_t::NONE) {
        bin64_t hint = hint_in_.front().bin;
        tint time = hint_in_.front().time;
        hint_in_.pop_front();
        while (!hint.is_base()) { // FIXME optimize; possible attack
            hint_in_.push_front(tintbin(time,hint.right()));
            hint = hint.left();
        }
        //if (time < NOW-TINT_SEC*3/2 )
        //    continue;  bad idea
        if (ack_in_.get(hint)!=binmap_t::FILLED) 
            send = hint;
    }
    uint64_t mass = 0;
    for(int i=0; i<hint_in_.size(); i++)
        mass += hint_in_[i].bin.width();
    dprintf("%s #%u dequeued %s [%lli]\n",tintstr(),id_,send.str(),mass);
    return send;
}


void    Channel::AddHandshake (Datagram& dgram) {
    if (!peer_channel_id_) { // initiating
        dgram.Push8(SWIFT_HASH);
        dgram.Push32(bin64_t::ALL32);
        dgram.PushHash(file().root_hash());
        dprintf("%s #%u +hash ALL %s\n",
                tintstr(),id_,file().root_hash().hex().c_str());
    }
    dgram.Push8(SWIFT_HANDSHAKE);
    int encoded = EncodeID(id_);
    dgram.Push32(encoded);
    dprintf("%s #%u +hs %x\n",tintstr(),id_,encoded);
    ack_out_.clear();
    AddAck(dgram);
}


void    Channel::Send () {
    Datagram dgram(socket_,peer());
    dgram.Push32(peer_channel_id_);
    bin64_t data = bin64_t::NONE;
    if ( is_established() ) {
        // FIXME: seeder check
        AddAck(dgram);
        if (!file().is_complete())
            AddHint(dgram);
        AddPex(dgram);
        CleanDataOut();
        data = AddData(dgram);
    } else {
        AddHandshake(dgram);
        AddAck(dgram);
    }
    dprintf("%s #%u sent %ib %s:%x\n",
            tintstr(),id_,dgram.size(),peer().str(),peer_channel_id_);
    if (dgram.size()==4) {// only the channel id; bare keep-alive
        data = bin64_t::ALL;
        //dprintf("%s #%u considering keepalive %i %f %s\n",
        //        tintstr(),id_,(int)data_out_.size(),cwnd_,SEND_CONTROL_MODES[send_control_]);
        //if (data_out_.size()<cwnd_ && send_control_!=KEEP_ALIVE_CONTROL) {
            //if ( cwnd_ < 1 )
            //    SwitchSendControl(KEEP_ALIVE_CONTROL);
            //else
            //    cwnd_ = cwnd_/2.0;
        //}
        //if (data_out_.empty() && send_control_!=KEEP_ALIVE_CONTROL)
        //     SwitchSendControl(KEEP_ALIVE_CONTROL);// we did our best
        //if (NOW<last_send_time_+MAX_SEND_INTERVAL) // no need for keepalive
        //    return; // don't send empty dgram
    }
    if (dgram.Send()==-1)
        print_error("can't send datagram");
    last_send_time_ = NOW;
    sent_since_recv_++;
    dgrams_sent_++;
}


void    Channel::AddHint (Datagram& dgram) {

    tint plan_for = max(TINT_SEC,rtt_avg_*4);
    
    tint timed_out = NOW - plan_for*2;
    while ( !hint_out_.empty() && hint_out_.front().time < timed_out ) {
        hint_out_size_ -= hint_out_.front().bin.width();
        hint_out_.pop_front();
    }
    
    /*int peer_cwnd = (int)(rtt_avg_ / dip_avg_);
    if (!peer_cwnd)
        peer_cwnd = 1;*/
    int plan_pck = std::max ( (tint)1, plan_for / dip_avg_ );
    
    if ( hint_out_size_ < plan_pck ) {
            
        int diff = plan_pck - hint_out_size_; // TODO: aggregate
        bin64_t hint = transfer().picker().Pick(ack_in_,diff,NOW+plan_for*2);
        
        if (hint!=bin64_t::NONE) {
            dgram.Push8(SWIFT_HINT);
            dgram.Push32(hint);
            dprintf("%s #%u +hint %s [%lli]\n",tintstr(),id_,hint.str(),hint_out_size_);
            hint_out_.push_back(hint);
            hint_out_size_ += hint.width();
        } else
            dprintf("%s #%u Xhint\n",tintstr(),id_);
        
    }
}


bin64_t        Channel::AddData (Datagram& dgram) {
    
    if (!file().size()) // know nothing
        return bin64_t::NONE;
    
    bin64_t tosend = bin64_t::NONE;
    if (data_out_.size()<cwnd_ && last_data_out_time_<=NOW-send_interval_) {
        tosend = DequeueHint();
        if (tosend==bin64_t::NONE) {
            dprintf("%s #%u no idea what to send #sendctrl\n",tintstr(),id_);
            if (send_control_!=KEEP_ALIVE_CONTROL)
                SwitchSendControl(KEEP_ALIVE_CONTROL);
        }
    } else
        dprintf("%s #%u no cwnd #sendctrl\n",tintstr(),id_);
    
    if (tosend==bin64_t::NONE)// && (last_data_out_time_>NOW-TINT_SEC || data_out_.empty())) 
        return bin64_t::NONE; // once in a while, empty data is sent just to check rtt FIXED
    
    if (ack_in_.is_empty() && file().size())
        AddPeakHashes(dgram);
    AddUncleHashes(dgram,tosend);
    if (!ack_in_.is_empty()) // TODO: cwnd_>1
        data_out_cap_ = tosend;

    if (dgram.size()>254) {
        dgram.Send(); // kind of fragmentation
        dgram.Push32(peer_channel_id_);
    }
    
    dgram.Push8(SWIFT_DATA);
    dgram.Push32(tosend.to32());
    
    uint8_t buf[1024];
    size_t r = pread(file().file_descriptor(),buf,1024,tosend.base_offset()<<10); 
    // TODO: corrupted data, retries, caching
    if (r<0) {
        print_error("error on reading");
        return bin64_t::NONE;
    }
    assert(dgram.space()>=r+4+1);
    dgram.Push(buf,r);
    
    last_data_out_time_ = NOW;
    data_out_.push_back(tosend);
    dprintf("%s #%u +data %s\n",tintstr(),id_,tosend.str());
    
    return tosend;
}


void    Channel::AddTs (Datagram& dgram) {
    dgram.Push8(SWIFT_TS);
    dgram.Push64(data_in_.time);
    dprintf("%s #%u +ts %s\n",tintstr(),id_,tintstr(data_in_.time));
}


void    Channel::AddAck (Datagram& dgram) {
    if (data_in_dbl_!=bin64_t::NONE) { // TODO: do redundancy better
        dgram.Push8(SWIFT_ACK);
        dgram.Push32(data_in_dbl_.to32());
        data_in_dbl_=bin64_t::NONE;
    }
    if (data_in_.time!=TINT_NEVER) { // TODO: ACK NONE for corrupted data
        AddTs(dgram);
        bin64_t pos = file().ack_out().cover(data_in_.bin);
        dgram.Push8(SWIFT_ACK);
        dgram.Push32(pos.to32());
        //dgram.Push64(data_in_.time);
        ack_out_.set(pos);
        dprintf("%s #%u +ack %s %s\n",tintstr(),id_,pos.str(),tintstr(data_in_.time));
        data_in_ = tintbin(TINT_NEVER,bin64_t::NONE);
        if (pos.layer()>2)
            data_in_dbl_ = pos;
    }
    for(int count=0; count<4; count++) {
        bin64_t ack = file().ack_out().find_filtered(ack_out_, bin64_t::ALL, binmap_t::FILLED);
        if (ack==bin64_t::NONE)
            break;
        ack = file().ack_out().cover(ack);
        ack_out_.set(ack);
        dgram.Push8(SWIFT_ACK);
        dgram.Push32(ack.to32());
        dprintf("%s #%u +ack %s\n",tintstr(),id_,ack.str());
    }
}


void    Channel::Recv (Datagram& dgram) {
    dprintf("%s #%u recvd %i\n",tintstr(),id_,dgram.size()+4);
    peer_send_time_ = 0; // has scope of 1 datagram
    dgrams_rcvd_++;
    if (last_send_time_ && rtt_avg_==TINT_SEC && dev_avg_==0) {
        rtt_avg_ = NOW - last_send_time_;
        dev_avg_ = rtt_avg_;
        dip_avg_ = rtt_avg_;
        dprintf("%s #%u rtt init %lli\n",tintstr(),id_,rtt_avg_);
    }
    bin64_t data = dgram.size() ? bin64_t::NONE : bin64_t::ALL;
    while (dgram.size()) {
        uint8_t type = dgram.Pull8();
        switch (type) {
            case SWIFT_HANDSHAKE: OnHandshake(dgram); break;
            case SWIFT_DATA:      data=OnData(dgram); break;
            case SWIFT_TS:        OnTs(dgram); break;
            case SWIFT_ACK:       OnAck(dgram); break;
            case SWIFT_HASH:      OnHash(dgram); break;
            case SWIFT_HINT:      OnHint(dgram); break;
            case SWIFT_PEX_ADD:   OnPex(dgram); break;
            default:
                eprintf("%s #%u ?msg id unknown %i\n",tintstr(),id_,(int)type);
                return;
        }
    }
    last_recv_time_ = NOW;
    sent_since_recv_ = 0;
}


void    Channel::OnHash (Datagram& dgram) {
    bin64_t pos = dgram.Pull32();
    Sha1Hash hash = dgram.PullHash();
    file().OfferHash(pos,hash);
    //DLOG(INFO)<<"#"<<id<<" .HASH"<<(int)pos;
    dprintf("%s #%u -hash %s\n",tintstr(),id_,pos.str());
}


void    Channel::CleanHintOut (bin64_t pos) {
    int hi = 0;
    while (hi<hint_out_.size() && !pos.within(hint_out_[hi].bin))
        hi++;
    if (hi==hint_out_.size())
        return; // something not hinted or hinted in far past
    while (hi--) { // removing likely snubbed hints
        hint_out_size_ -= hint_out_.front().bin.width();
        hint_out_.pop_front();
    }
    while (hint_out_.front().bin!=pos) {
        tintbin f = hint_out_.front();
        f.bin = f.bin.towards(pos);
        hint_out_.front().bin = f.bin.sibling();
        hint_out_.push_front(f);
    }
    hint_out_.pop_front();
    hint_out_size_--;
}


bin64_t Channel::OnData (Datagram& dgram) {
    bin64_t pos = dgram.Pull32();
    uint8_t *data;
    int length = dgram.Pull(&data,1024);
    bool ok = (pos==bin64_t::NONE) || file().OfferData(pos, (char*)data, length) ;
    dprintf("%s #%u %cdata %s\n",tintstr(),id_,ok?'-':'!',pos.str());
    data_in_ = tintbin(NOW,bin64_t::NONE);
    if (!ok) 
        return bin64_t::NONE;
    data_in_.bin = pos;
    if (pos!=bin64_t::NONE) {
        if (last_data_in_time_) {
            tint dip = NOW - last_data_in_time_;
            dip_avg_ = ( dip_avg_*3 + dip ) >> 2;
        }
        last_data_in_time_ = NOW;
    }
    CleanHintOut(pos);    
    return pos;
}


void    Channel::CleanDataOut (bin64_t ackd_pos) { // TODO: isn't it too long?
    
    int max_ack_off = 0;
    //FIXME do LEDBAT magic somewhere here
    
    if (ackd_pos!=bin64_t::NONE) {
        for (int i=0; i<8 && i<data_out_.size(); i++) {
            if (data_out_[i]!=tintbin() && data_out_[i].bin.within(ackd_pos)) {
                tint rtt = NOW-data_out_[i].time;
                rtt_avg_ = (rtt_avg_*7 + rtt) >> 3;
                dev_avg_ = ( dev_avg_*3 + abs(rtt-rtt_avg_) ) >> 2;
                if (peer_send_time_) {
                    assert(data_out_[i].time!=TINT_NEVER);
                    tint owd = peer_send_time_ - data_out_[i].time;
                    owd_cur_bin_ = (owd_cur_bin_+1) & 3;
                    owd_current_[owd_cur_bin_] = owd;
                    if ( owd_min_bin_start_+TINT_SEC*30 < NOW ) {
                        owd_min_bin_start_ = NOW;
                        owd_min_bin_ = (owd_min_bin_+1) & 3;
                        owd_min_bins_[owd_min_bin_] = TINT_NEVER;
                    }
                    if (owd_min_bins_[owd_min_bin_]>owd)
                        owd_min_bins_[owd_min_bin_] = owd;
                }
                dprintf("%s #%u rtt %lli dev %lli\n",tintstr(),id_,rtt_avg_,dev_avg_);
                bin64_t pos = data_out_[i].bin;
                ack_rcvd_recent_++;
                data_out_[i]=tintbin();
                max_ack_off = i;
                if (ackd_pos==pos)
                    break;
            }
        }
        while (!data_out_.empty() && data_out_.front().bin==bin64_t::NONE) {
            data_out_.pop_front();
            max_ack_off--;
        }
        static const int MAX_REORDERING = 2;  // the triple-ACK principle
        if (max_ack_off>MAX_REORDERING) {
            while (max_ack_off && (data_out_.front().bin==bin64_t::NONE
                                   || ack_in_.is_filled(data_out_.front().bin)) ) {
                data_out_.pop_front();
                max_ack_off--;
            }
            while (max_ack_off>MAX_REORDERING) {
                ack_not_rcvd_recent_++;
                dprintf("%s #%u Rdata %s\n",tintstr(),id_,data_out_.front().bin.str());
                data_out_.pop_front();
                max_ack_off--;
                data_out_cap_ = bin64_t::ALL;
            }
        }
    }
    tint timeout = NOW - rtt_avg_ - 4*std::max(dev_avg_,TINT_MSEC*50);
    while (!data_out_.empty() && data_out_.front().time<timeout) {
        if (data_out_.front().bin!=bin64_t::NONE && ack_in_.is_empty(data_out_.front().bin)) {
            ack_not_rcvd_recent_++;
            data_out_cap_ = bin64_t::ALL;
            dprintf("%s #%u Tdata %s\n",tintstr(),id_,data_out_.front().bin.str());
        }
        data_out_.pop_front();
    }
    while (!data_out_.empty() && data_out_.front().bin==bin64_t::NONE)
        data_out_.pop_front();
    assert(data_out_.empty() || data_out_.front().time!=TINT_NEVER);

}


void    Channel::OnAck (Datagram& dgram) {
    bin64_t ackd_pos = dgram.Pull32();
    if (ackd_pos==bin64_t::NONE)
        return; // likely, brocken packet / insufficient hashes
    if (file().size() && ackd_pos.base_offset()>=file().packet_size()) {
        eprintf("invalid ack: %s\n",ackd_pos.str());
        return;
    }
    dprintf("%s #%u -ack %s\n",tintstr(),id_,ackd_pos.str());
    ack_in_.set(ackd_pos);
    CleanDataOut(ackd_pos); // FIXME do AFTER all ACKs
}


void Channel::OnTs (Datagram& dgram) {
    peer_send_time_ = dgram.Pull64();
    dprintf("%s #%u -ts %lli\n",tintstr(),id_,peer_send_time_);
}


void    Channel::OnHint (Datagram& dgram) {
    bin64_t hint = dgram.Pull32();
    hint_in_.push_back(hint);
    //ack_in_.set(hint,binmap_t::EMPTY);
    //RequeueSend(cc_->OnHintRecvd(hint));
    dprintf("%s #%u -hint %s\n",tintstr(),id_,hint.str());
}


void Channel::OnHandshake (Datagram& dgram) {
    peer_channel_id_ = dgram.Pull32();
    dprintf("%s #%u -hs %x\n",tintstr(),id_,peer_channel_id_);
    // self-connection check
    if (!SELF_CONN_OK) {
        uint32_t try_id = DecodeID(peer_channel_id_);
        if (channel(try_id) && !channel(try_id)->peer_channel_id_) {
            peer_channel_id_ = 0;
            Close();
            return; // this is a self-connection
        }
    }
    // FUTURE: channel forking
}


void Channel::OnPex (Datagram& dgram) {
    uint32_t ipv4 = dgram.Pull32();
    uint16_t port = dgram.Pull16();
    Address addr(ipv4,port);
    dprintf("%s #%u -pex %s\n",tintstr(),id_,addr.str());
    transfer().OnPexIn(addr);
}


void    Channel::AddPex (Datagram& dgram) {
    int chid = transfer().RevealChannel(pex_out_);
    if (chid==-1 || chid==id_)
        return;
    Address a = channels[chid]->peer();
    dgram.Push8(SWIFT_PEX_ADD);
    dgram.Push32(a.ipv4());
    dgram.Push16(a.port());
    dprintf("%s #%u +pex %s\n",tintstr(),id_,a.str());
}


Channel*    Channel::RecvDatagram (int socket) {
    Datagram data(socket);
    data.Recv();
    const Address& addr = data.address();
#define return_log(...) { printf(__VA_ARGS__); return NULL; }
    if (data.size()<4) 
        return_log("datagram shorter than 4 bytes %s\n",addr.str());
    uint32_t mych = data.Pull32();
    Sha1Hash hash;
    Channel* channel = NULL;
    if (!mych) { // handshake initiated
        if (data.size()<1+4+1+4+Sha1Hash::SIZE) 
            return_log ("%s #0 incorrect size %i initial handshake packet %s\n",
                        tintstr(),data.size(),addr.str());
        uint8_t hashid = data.Pull8();
        if (hashid!=SWIFT_HASH) 
            return_log ("%s #0 no hash in the initial handshake %s\n",
                        tintstr(),addr.str());
        bin64_t pos = data.Pull32();
        if (pos!=bin64_t::ALL) 
            return_log ("%s #0 that is not the root hash %s\n",tintstr(),addr.str());
        hash = data.PullHash();
        FileTransfer* file = FileTransfer::Find(hash);
        if (!file) 
            return_log ("%s #0 hash %s unknown, no such file %s\n",tintstr(),hash.hex().c_str(),addr.str());
        dprintf("%s #0 -hash ALL %s\n",tintstr(),hash.hex().c_str());
        for(binqueue::iterator i=file->hs_in_.begin(); i!=file->hs_in_.end(); i++)
            if (channels[*i] && channels[*i]->peer_==data.address() && 
                channels[*i]->last_recv_time_>NOW-TINT_SEC*2)
                return_log("%s #0 have a channel already to %s\n",tintstr(),addr.str());
        channel = new Channel(file, socket, data.address());
    } else {
        mych = DecodeID(mych);
        if (mych>=channels.size()) 
            return_log("%s invalid channel #%u, %s\n",tintstr(),mych,addr.str());
        channel = channels[mych];
        if (!channel) 
            return_log ("%s #%u is already closed\n",tintstr(),mych,addr.str());
        if (channel->peer() != addr) 
            return_log ("%s #%u invalid peer address %s!=%s\n",
                        tintstr(),mych,channel->peer().str(),addr.str());
        channel->own_id_mentioned_ = true;
    }
    //dprintf("recvd %i bytes for %i\n",data.size(),channel->id);
    channel->Recv(data);
    return channel;
}


void    Channel::Loop (tint howlong) {  
    
    tint limit = Datagram::Time() + howlong;
    
    do {

        tint send_time(TINT_NEVER);
        Channel* sender(NULL);
        while (!sender && !send_queue.is_empty()) { // dequeue
            tintbin next = send_queue.pop();
            sender = channel((int)next.bin);
            send_time = next.time;
            if (sender && sender->next_send_time_!=send_time &&
                     sender->next_send_time_!=TINT_NEVER )
                sender = NULL; // it was a stale entry
        }
        
        if ( sender!=NULL && send_time<=NOW ) { // it's time
            
            dprintf("%s #%u sch_send %s\n",tintstr(),sender->id(),
                    tintstr(send_time));
            sender->Send();
            sender->Reschedule();
            
        } else {  // it's too early, wait
            
            tint towait = min(limit,send_time) - NOW;
            dprintf("%s waiting %lliusec\n",tintstr(),towait);
            int rd = Datagram::Wait(socket_count,sockets,towait);
            if (rd!=INVALID_SOCKET) { // in meantime, received something
                Channel* receiver = RecvDatagram(rd);
                if (receiver) // receiver's state may have changed
                    receiver->Reschedule();
            }
            if (sender)  // get back to that later
                send_queue.push(tintbin(send_time,sender->id()));
            
        }
        
    } while (NOW<limit);
            
}

 
void Channel::Close () {
    this->SwitchSendControl(CLOSE_CONTROL);
}


void Channel::Reschedule () {
    next_send_time_ = NextSendTime();
    if (next_send_time_!=TINT_NEVER) {
        assert(next_send_time_<NOW+TINT_MIN);
        send_queue.push(tintbin(next_send_time_,id_));
        dprintf("%s requeue #%u for %s\n",tintstr(),id_,tintstr(next_send_time_));
    } else {
        dprintf("%s #%u closed\n",tintstr(),id_);
        delete this;
    }
}
