#pragma once
#include "native_carrier.hpp"
namespace xgtx_exchange {
inline bytes reserve_wallet_carrier(const bytes& extra,const envelope& intent){
  auto fields=carrier_fields(extra);
  for(const auto& field:fields)carrier_require(field.type()!=typeid(cryptonote::tx_extra_xgtx_exchange),"existing wallet carrier");
  carrier_require(cryptonote_verify(intent.op.sender,signing_message(intent.op),intent.sig),"invalid reserved intent");
  cryptonote::tx_extra_xgtx_exchange field;field.data=encode_carrier(native_carrier{intent,{}});
  auto padding=std::find_if(fields.begin(),fields.end(),[](const auto& f){return f.type()==typeid(cryptonote::tx_extra_padding);});
  fields.insert(padding,field);return serialize_carrier_fields(fields);
}
inline bytes remove_reserved_carrier(const bytes& extra,const envelope& intent){
  auto fields=carrier_fields(extra);auto index=require_carrier_index(fields);
  auto carrier=decode_carrier(boost::get<cryptonote::tx_extra_xgtx_exchange>(fields[index]).data);
  carrier_require(carrier.binding==signature{} && encode_envelope(carrier.intent)==encode_envelope(intent),"reserved intent changed");
  fields.erase(fields.begin()+index);return serialize_carrier_fields(fields);
}
}
