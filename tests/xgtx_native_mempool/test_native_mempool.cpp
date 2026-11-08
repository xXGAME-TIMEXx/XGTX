#include "../../src/xgtx_exchange/native_mempool.hpp"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <iostream>
using namespace xgtx_exchange;
namespace {
int checks=0;
void check(bool ok,const char* name){++checks;if(!ok)throw std::runtime_error(name);}
template<class F>void reject(F f,const char* name){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,name);}
id key(unsigned char value){id a{};a.fill(value);return a;}
cryptonote::transaction skeleton(const xgtx_test::wallet& owner){
  cryptonote::transaction tx;tx.version=2;
  cryptonote::txin_to_key input;input.key_offsets.assign(16,1);std::memcpy(&input.k_image,owner.pub.data(),32);tx.vin.push_back(input);
  cryptonote::tx_out output;output.amount=0;output.target=cryptonote::txout_to_key(owner.public_key);tx.vout.assign(2,output);
  tx.rct_signatures.type=rct::RCTTypeCLSAG;tx.rct_signatures.txnFee=1000;tx.rct_signatures.outPk.resize(2);tx.rct_signatures.ecdhInfo.resize(2);
  for(auto& item:tx.rct_signatures.outPk)std::memcpy(&item.mask,owner.pub.data(),32);
  cryptonote::add_tx_pub_key_to_extra(tx,owner.public_key);return tx;
}
cryptonote::transaction carrier(const xgtx_test::wallet& owner,const envelope& intent){auto tx=skeleton(owner);seal_native_carrier(tx,intent,owner.public_key,owner.secret_key);return tx;}
}
int main(){try{
  xgtx_test::wallet alice(1),bob(2);auto chain=key(0x47);xgtx_exchange::config cfg{chain,bob.pub,2,1024};
  engine e(cfg,cryptonote_hash,cryptonote_verify);state empty;auto confirmed=e.apply_block(empty,1,id{},key(1),{}).next;
  operation op;op.chain=chain;op.deadline=100;op.quantity=1000000;op.ticker="GAME";
  auto issue=alice.sign(op);auto tx=carrier(alice,issue);auto original=serialize(confirmed);
  check(!transaction_intent(skeleton(alice),chain),"ordinary native transaction has no intent");
  check(encode_envelope(*transaction_intent(tx,chain))==encode_envelope(issue),"binding verified before admission");
  validate_admission(e,confirmed,issue,key(2),{});check(serialize(confirmed)==original,"admission leaves confirmed state unchanged");
  std::vector<pending_operation_record> pending{{key(3),issue.op}};
  reject([&]{validate_admission(e,confirmed,issue,key(2),pending);},"different native tx conflicts on owner nonce");
  validate_admission(e,confirmed,issue,key(3),pending);check(true,"same transaction relay update permitted");
  pending.clear();validate_admission(e,confirmed,issue,key(2),pending);check(true,"removed transaction frees nonce immediately");
  auto stale=issue.op;stale.nonce=9;pending={{key(3),stale}};validate_admission(e,confirmed,issue,key(2),pending);check(true,"unrelated nonce does not pin owner");
  auto other=issue.op;other.sender=bob.pub;pending={{key(3),other}};validate_admission(e,confirmed,issue,key(2),pending);check(true,"different owner independent");
  other=issue.op;other.chain=key(0x48);pending={{key(3),other}};validate_admission(e,confirmed,issue,key(2),pending);check(true,"different chain independent");
  auto future=op;future.nonce=2;auto future_intent=alice.sign(future);reject([&]{validate_admission(e,confirmed,future_intent,key(2),{});},"pending credits/nonces cannot fund admission");
  auto expired=op;expired.deadline=1;auto expired_intent=alice.sign(expired);reject([&]{validate_admission(e,confirmed,expired_intent,key(2),{});},"expired at next block ordinal");
  auto later=cfg;later.activation_height=3;engine gated(later,cryptonote_hash,cryptonote_verify);reject([&]{validate_admission(gated,confirmed,issue,key(2),{});},"preactivation admission rejected");
  auto changed=tx;changed.rct_signatures.txnFee++;reject([&]{transaction_intent(changed,chain);},"modified native binding rejected");
  auto mined=e.apply_block(confirmed,2,key(1),key(4),{issue}).next;
  reject([&]{validate_admission(e,mined,issue,key(2),{});},"confirmed replay rejected");
  validate_admission(e,confirmed,issue,key(2),{});check(true,"reorg restores original valid nonce from confirmed state");
  template_view view(e,confirmed);check(view.include(tx),"issuance selected");auto selected=serialize(view.candidate());
  reject([&]{view.include(tx);},"template nonce replay rejected");check(serialize(view.candidate())==selected && view.operations()==1,"rejected template candidate atomic");
  auto asset=e.asset_id(issue.op);operation transfer;transfer.type=kind::transfer;transfer.chain=chain;transfer.nonce=2;transfer.deadline=100;transfer.asset=asset;transfer.recipient=bob.pub;transfer.quantity=100;
  auto transfer_intent=alice.sign(transfer);auto transfer_tx=carrier(alice,transfer_intent);
  reject([&]{validate_admission(e,confirmed,transfer_intent,key(5),{});},"relay does not accept dependent unconfirmed transfer");
  check(view.include(transfer_tx),"template permits consensus-valid ordered dependency");
  check(view.candidate().balances.at({bob.pub,asset})==100,"template applies preceding issuance and transfer");
  check(!view.include(skeleton(alice)) && view.operations()==2,"ordinary transaction consumes no exchange operation slot");
  auto before=serialize(view.candidate());changed=transfer_tx;changed.unlock_time++;reject([&]{view.include(changed);},"template tampered binding rejected");check(serialize(view.candidate())==before,"binding failure atomic");
  auto one=cfg;one.max_operations=1;engine capped(one,cryptonote_hash,cryptonote_verify);template_view capped_view(capped,confirmed);capped_view.include(tx);
  auto bob_issue=bob.sign(op);auto bob_tx=carrier(bob,bob_issue);reject([&]{capped_view.include(bob_tx);},"template operation cap");check(capped_view.operations()==1,"operation cap does not consume slot");
  auto malformed=tx;malformed.extra.push_back(0xff);reject([&]{transaction_intent(malformed,chain);},"malformed extra rejected");
  auto duplicate=carrier_fields(tx.extra);duplicate.push_back(duplicate[require_carrier_index(duplicate)]);malformed=tx;malformed.extra=serialize_carrier_fields(duplicate);reject([&]{pending_operation(malformed);},"duplicate pending carrier rejected");
  check(serialize(confirmed)==original,"all admission/template previews leave confirmed snapshot unchanged");
  // Two individually valid swaps: the first changes the price enough that the
  // second must be skipped at its original exact minimum output.
  auto pool_cfg=cfg;pool_cfg.activation_height=1;engine pe(pool_cfg,cryptonote_hash,cryptonote_verify);state funded;
  auto aop=op;aop.ticker="ALPHA";aop.quantity=1000000;auto ae=alice.sign(aop);auto ar=pe.apply(funded,ae,1);
  auto bop=op;bop.nonce=2;bop.ticker="BETA";bop.quantity=1000000;auto be=alice.sign(bop);auto br=pe.apply(funded,be,1);
  operation pool;pool.type=kind::pool_create;pool.chain=chain;pool.nonce=3;pool.deadline=100;pool.asset=ar.asset;pool.other=br.asset;pool.quantity=500000;pool.limit=500000;pe.apply(funded,alice.sign(pool),1);
  operation credit;credit.type=kind::transfer;credit.chain=chain;credit.nonce=4;credit.deadline=100;credit.asset=ar.asset;credit.quantity=100000;credit.recipient=bob.pub;pe.apply(funded,alice.sign(credit),1);
  funded=pe.apply_block(funded,1,id{},key(8),{}).next;
  operation swap;swap.type=kind::swap;swap.chain=chain;swap.nonce=5;swap.deadline=100;swap.asset=ar.asset;swap.other=br.asset;swap.quantity=100000;swap.minimum=1;
  auto first=alice.sign(swap);auto bob_swap=swap;bob_swap.nonce=1;auto preview=funded;auto quote=pe.apply(preview,bob.sign(bob_swap),2);bob_swap.minimum=quote.output;auto second=bob.sign(bob_swap);
  validate_admission(pe,funded,first,key(9),{});validate_admission(pe,funded,second,key(10),{});check(true,"competing swaps individually valid against confirmed reserves");
  template_view prices(pe,funded);prices.include(carrier(alice,first));auto after_first=serialize(prices.candidate());
  reject([&]{prices.include(carrier(bob,second));},"template skips swap invalidated by earlier price movement");
  check(serialize(prices.candidate())==after_first && prices.operations()==1,"failed slippage does not alter selected swaps");
  std::cout<<checks<<" checks passed: admission nonce conflicts, confirmed-only balances, reorg views, ordered template simulation, slippage, cap, atomic preview\n";
  std::cout<<"Policy helper tests with unsigned native skeletons; actual node relay/consensus and funded RingCT transfers remain unverified.\n";
  return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL after "<<checks<<" checks: "<<ex.what()<<"\n";return 1;}}
