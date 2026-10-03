#pragma once
#include <khz/khz_client_input_checkpoint.h>

namespace khz::input_replay {
inline bool EndReceiptGuards(bool replayActive,bool producerParkedAndDrained,std::uint64_t& sequence,std::uint64_t& qpc){
    if(!replayActive||!producerParkedAndDrained)return false;
    // Replay receipt numbers are synthetic. The hardware producer retains its
    // own monotonic sequence and must not wait to catch up with replay receipts.
    sequence=qpc=0;return true;
}
inline bool ValidFields(const KHZ_ClientInputReceipt& receipt){
    if(receipt.tickOffset>60000||receipt.microseconds>=1000)return false;
    const auto& event=receipt.event;
    if(event.kind==KHZ_OwnedInputKind::Reset)return unsigned(event.resetReason)<=unsigned(KHZ_InputResetReason::Stop);
    if(event.kind==KHZ_OwnedInputKind::Key)return event.virtualKey<256&&event.scanCode<=255&&!(event.keyboardFlags&~7u);
    return event.kind==KHZ_OwnedInputKind::Mouse&&!(event.mouseButtonFlags&~0xFFFu)&&event.wheelDelta>=-32768&&event.wheelDelta<=32767;
}
inline void ReceiptIdentity(canonical::Hash& hash,const KHZ_ClientInputReceipt& receipt){
    hash.u64(receipt.tickOffset);hash.scalar(receipt.microseconds);const auto& event=receipt.event;hash.scalar(event.kind);
    if(event.kind==KHZ_OwnedInputKind::Reset)hash.scalar(event.resetReason);
    else if(event.kind==KHZ_OwnedInputKind::Key){hash.scalar(event.virtualKey);hash.scalar(event.scanCode);hash.scalar(event.keyboardFlags);hash.boolean(event.keyDown);}
    else{hash.scalar(event.dx);hash.scalar(event.dy);hash.scalar(event.mouseButtonFlags);hash.scalar(event.wheelDelta);}
}
inline bool CheckpointShape(const KHZ_ClientInputCheckpoint& value){
    if(value.schema!=1||!value.initialized||value.typedInput||value.mainPending||!value.allReleased
        ||!value.controlEpoch||value.locationSelection!=0||!value.bindingIdentity.valid)return false;
    for(const auto& button:value.buttons)if(button.down[0]||button.down[1]||button.active||button.wasPressed||button.msec)return false;
    for(bool held:value.keyDown)if(held)return false;
    for(bool waiting:value.awaitRelease)if(waiting)return false;
    return true;
}
}
