#include <input_replay_policy.hpp>
#include <client_checkpoint.hpp>
#include <cstdio>

namespace {
khz::canonical::Digest receipt(const KHZ_ClientInputReceipt& value){khz::canonical::Hash hash;khz::input_replay::ReceiptIdentity(hash,value);return hash.finish();}
}
int main(){
    unsigned cases=0,failures=0;const auto check=[&](bool passed){++cases;if(!passed)++failures;};
    KHZ_ClientInputCheckpoint checkpoint{};checkpoint.schema=1;checkpoint.initialized=true;checkpoint.allReleased=true;checkpoint.controlEpoch=1;
    khz::canonical::Hash binding;binding.text("fixture frozen identity");checkpoint.bindingIdentity=binding.finish();
    check(khz::input_replay::CheckpointShape(checkpoint));
    for(unsigned index=0;index<30;++index)for(unsigned field=0;field<5;++field){
        auto changed=checkpoint;switch(field){case 0:changed.buttons[index].down[0]=4;break;case 1:changed.buttons[index].down[1]=5;break;
            case 2:changed.buttons[index].active=1;break;case 3:changed.buttons[index].wasPressed=1;break;case 4:changed.buttons[index].msec=1;break;}
        check(!khz::input_replay::CheckpointShape(changed));
    }
    for(unsigned index=0;index<256;++index){auto changed=checkpoint;changed.keyDown[index]=true;check(!khz::input_replay::CheckpointShape(changed));
        changed=checkpoint;changed.awaitRelease[index]=true;check(!khz::input_replay::CheckpointShape(changed));}
    for(unsigned field=0;field<7;++field){auto changed=checkpoint;switch(field){case 0:changed.schema=2;break;case 1:changed.initialized=false;break;
        case 2:changed.typedInput=true;break;case 3:changed.mainPending=true;break;case 4:changed.controlEpoch=0;break;case 5:changed.locationSelection=1;break;case 6:changed.bindingIdentity.valid=false;break;}
        check(!khz::input_replay::CheckpointShape(changed));}
    KHZ_ClientInputReceipt key{};key.tickOffset=17;key.microseconds=999;key.event.kind=KHZ_OwnedInputKind::Key;key.event.virtualKey=87;key.event.scanCode=17;key.event.keyDown=true;
    check(khz::input_replay::ValidFields(key));const auto original=receipt(key);
    auto moved=key;moved.event.qpc=123456;moved.event.sequence=555;moved.event.controlEpoch=777;moved.event.messageTimeMs=42;
    check(khz::canonical::equal(original,receipt(moved))); // timeline guards are supplied separately
    moved=key;moved.event.keyDown=false;check(!khz::canonical::equal(original,receipt(moved)));
    moved=key;moved.tickOffset++;check(!khz::canonical::equal(original,receipt(moved)));
    moved=key;moved.microseconds--;check(!khz::canonical::equal(original,receipt(moved)));
    moved=key;moved.event.virtualKey=256;check(!khz::input_replay::ValidFields(moved));
    moved=key;moved.event.scanCode=256;check(!khz::input_replay::ValidFields(moved));
    moved=key;moved.event.keyboardFlags=8;check(!khz::input_replay::ValidFields(moved));
    moved=key;moved.tickOffset=60001;check(!khz::input_replay::ValidFields(moved));
    moved=key;moved.microseconds=1000;check(!khz::input_replay::ValidFields(moved));
    KHZ_ClientInputReceipt mouse{};mouse.event.kind=KHZ_OwnedInputKind::Mouse;mouse.event.dx=-43;mouse.event.wheelDelta=-32768;
    check(khz::input_replay::ValidFields(mouse));mouse.event.wheelDelta=32768;check(!khz::input_replay::ValidFields(mouse));
    mouse.event.wheelDelta=0;mouse.event.mouseButtonFlags=0x1000;check(!khz::input_replay::ValidFields(mouse));
    KHZ_ClientInputReceipt reset{};reset.event.kind=KHZ_OwnedInputKind::Reset;reset.event.resetReason=KHZ_InputResetReason::Overflow;
    check(khz::input_replay::ValidFields(reset));reset.event.resetReason=static_cast<KHZ_InputResetReason>(99);check(!khz::input_replay::ValidFields(reset));
    std::uint64_t first,second;check(khz::checkpoint::receiptDeadline(100000,3579545,key.tickOffset,key.microseconds,&first));
    check(khz::checkpoint::receiptDeadline(900000,3579545,key.tickOffset,key.microseconds,&second)&&second-first==800000);
    check(!khz::checkpoint::receiptDeadline(UINT64_MAX,3579545,key.tickOffset,key.microseconds,&second));
    std::uint64_t syntheticSequence=100,syntheticQpc=900000;
    check(!khz::input_replay::EndReceiptGuards(true,false,syntheticSequence,syntheticQpc));
    check(syntheticSequence==100&&syntheticQpc==900000); // no reset without the actual producer fence
    check(!khz::input_replay::EndReceiptGuards(false,true,syntheticSequence,syntheticQpc));
    check(syntheticSequence==100&&syntheticQpc==900000);
    check(khz::input_replay::EndReceiptGuards(true,true,syntheticSequence,syntheticQpc));
    check(10>syntheticSequence&&800000>=syntheticQpc); // lower hardware sequence/QPC is admitted immediately
    std::printf("KHZ_INPUT_REPLAY_POLICY_TEST {\"cases\":%u,\"failures\":%u,\"scope\":\"released checkpoint shape, bounded semantic receipts, rational rebasing; actual worker restoration remains a native fixture gate\"}\n",cases,failures);
    return failures?1:0;
}
