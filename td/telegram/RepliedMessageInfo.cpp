//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/RepliedMessageInfo.h"

#include "td/telegram/ContactsManager.h"
#include "td/telegram/Dependencies.h"
#include "td/telegram/MessageContent.h"
#include "td/telegram/MessageFullId.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/misc.h"
#include "td/telegram/OptionManager.h"
#include "td/telegram/ScheduledServerMessageId.h"
#include "td/telegram/ServerMessageId.h"
#include "td/telegram/StoryId.h"
#include "td/telegram/Td.h"
#include "td/telegram/UserId.h"

#include "td/utils/logging.h"

namespace td {

static bool has_qts_messages(const Td *td, DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      return td->option_manager_->get_option_integer("session_count") > 1;
    case DialogType::Channel:
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }
}

RepliedMessageInfo::~RepliedMessageInfo() = default;

RepliedMessageInfo::RepliedMessageInfo(Td *td, tl_object_ptr<telegram_api::messageReplyHeader> &&reply_header,
                                       DialogId dialog_id, MessageId message_id, int32 date) {
  CHECK(reply_header != nullptr);
  if (reply_header->reply_to_scheduled_) {
    message_id_ = MessageId(ScheduledServerMessageId(reply_header->reply_to_msg_id_), date);
    if (message_id.is_valid_scheduled()) {
      auto reply_to_peer_id = std::move(reply_header->reply_to_peer_id_);
      if (reply_to_peer_id != nullptr) {
        dialog_id_ = DialogId(reply_to_peer_id);
        LOG(ERROR) << "Receive reply to " << MessageFullId{dialog_id_, message_id_} << " in "
                   << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
        dialog_id_ = DialogId();
      }
      if (message_id == message_id_) {
        LOG(ERROR) << "Receive reply to " << message_id_ << " in " << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
      }
    } else {
      LOG(ERROR) << "Receive reply to " << message_id_ << " in " << MessageFullId{dialog_id, message_id};
      message_id_ = MessageId();
    }
    if (reply_header->reply_from_ != nullptr || reply_header->reply_media_ != nullptr) {
      LOG(ERROR) << "Receive reply from other chat " << to_string(reply_header) << " in "
                 << MessageFullId{dialog_id, message_id};
    }
  } else {
    if (reply_header->reply_to_msg_id_ != 0) {
      message_id_ = MessageId(ServerMessageId(reply_header->reply_to_msg_id_));
      auto reply_to_peer_id = std::move(reply_header->reply_to_peer_id_);
      if (reply_to_peer_id != nullptr) {
        dialog_id_ = DialogId(reply_to_peer_id);
        if (!dialog_id_.is_valid()) {
          LOG(ERROR) << "Receive reply in invalid " << to_string(reply_to_peer_id);
          message_id_ = MessageId();
          dialog_id_ = DialogId();
        }
        if (dialog_id_ == dialog_id) {
          dialog_id_ = DialogId();  // just in case
        }
      }
      if (!message_id_.is_valid()) {
        LOG(ERROR) << "Receive " << to_string(reply_header) << " in " << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
        dialog_id_ = DialogId();
      } else if (!message_id.is_scheduled() && !dialog_id_.is_valid() &&
                 ((message_id_ > message_id && !has_qts_messages(td, dialog_id)) || message_id_ == message_id)) {
        LOG(ERROR) << "Receive reply to " << message_id_ << " in " << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
      }
    } else if (reply_header->reply_to_peer_id_ != nullptr) {
      LOG(ERROR) << "Receive " << to_string(reply_header) << " in " << MessageFullId{dialog_id, message_id};
    }
    if (reply_header->reply_from_ != nullptr) {
      origin_date_ = reply_header->reply_from_->date_;
      if (reply_header->reply_from_->channel_post_ != 0) {
        LOG(ERROR) << "Receive " << to_string(reply_header) << " in " << MessageFullId{dialog_id, message_id};
      } else {
        auto r_reply_origin = MessageOrigin::get_message_origin(td, std::move(reply_header->reply_from_));
        if (r_reply_origin.is_error()) {
          origin_date_ = 0;
        }
      }
    }
    if (reply_header->reply_media_ != nullptr &&
        reply_header->reply_media_->get_id() != telegram_api::messageMediaEmpty::ID) {
      content_ = get_message_content(td, FormattedText(), std::move(reply_header->reply_media_), dialog_id, true,
                                     UserId(), nullptr, nullptr, "messageReplyHeader");
      CHECK(content_ != nullptr);
      switch (content_->get_type()) {
        case MessageContentType::Animation:
        case MessageContentType::Audio:
        case MessageContentType::Contact:
        case MessageContentType::Dice:
        case MessageContentType::Document:
        // case MessageContentType::ExpiredPhoto:
        // case MessageContentType::ExpiredVideo:
        case MessageContentType::Game:
        case MessageContentType::Giveaway:
        case MessageContentType::Invoice:
        // case MessageContentType::LiveLocation:
        case MessageContentType::Location:
        case MessageContentType::Photo:
        case MessageContentType::Poll:
        case MessageContentType::Sticker:
        case MessageContentType::Story:
        // case MessageContentType::Text:
        case MessageContentType::Unsupported:
        case MessageContentType::Venue:
        case MessageContentType::Video:
        case MessageContentType::VideoNote:
        case MessageContentType::VoiceNote:
          break;
        default:
          LOG(ERROR) << "Receive reply with media of the type " << content_->get_type();
          content_ = nullptr;
      }
    }
  }
  if (!reply_header->quote_text_.empty()) {
    is_quote_manual_ = reply_header->quote_;
    auto entities = get_message_entities(td->contacts_manager_.get(), std::move(reply_header->quote_entities_),
                                         "RepliedMessageInfo");
    auto status = fix_formatted_text(reply_header->quote_text_, entities, true, true, true, true, false);
    if (status.is_error()) {
      if (!clean_input_string(reply_header->quote_text_)) {
        reply_header->quote_text_.clear();
      }
      entities.clear();
    }
    quote_ = FormattedText{std::move(reply_header->quote_text_), std::move(entities)};
  }
}

RepliedMessageInfo::RepliedMessageInfo(Td *td, const MessageInputReplyTo &input_reply_to) {
  if (!input_reply_to.message_id_.is_valid()) {
    return;
  }
  message_id_ = input_reply_to.message_id_;
}

bool RepliedMessageInfo::need_reget() const {
  return content_ != nullptr && need_reget_message_content(content_.get());
}

bool RepliedMessageInfo::need_reply_changed_warning(
    const RepliedMessageInfo &old_info, const RepliedMessageInfo &new_info, MessageId old_top_thread_message_id,
    bool is_yet_unsent, std::function<bool(const RepliedMessageInfo &info)> is_reply_to_deleted_message) {
  if (old_info.origin_date_ != new_info.origin_date_ && old_info.origin_date_ != 0 && new_info.origin_date_ != 0) {
    // date of the original message can't change
    return true;
  }
  if (old_info.origin_ != new_info.origin_ && !old_info.origin_.has_sender_signature() &&
      !new_info.origin_.has_sender_signature() && !old_info.origin_.is_empty() && !new_info.origin_.is_empty()) {
    // only signature can change in the message origin
    return true;
  }
  if (old_info.dialog_id_ != new_info.dialog_id_ && old_info.dialog_id_ != DialogId() &&
      new_info.dialog_id_ != DialogId()) {
    // reply chat can't change
    return true;
  }
  if (old_info.message_id_ == new_info.message_id_ && old_info.dialog_id_ == new_info.dialog_id_) {
    if (old_info.message_id_ != MessageId()) {
      if (old_info.origin_date_ != new_info.origin_date_) {
        // date of the original message can't change
        return true;
      }
      if (old_info.origin_ != new_info.origin_ && !old_info.origin_.has_sender_signature() &&
          !new_info.origin_.has_sender_signature()) {
        // only signature can change in the message origin
        return true;
      }
    }
    return false;
  }
  if (is_yet_unsent && is_reply_to_deleted_message(old_info) && new_info.message_id_ == MessageId()) {
    // reply to a deleted message, which was available locally
    return false;
  }
  if (is_yet_unsent && is_reply_to_deleted_message(new_info) && old_info.message_id_ == MessageId()) {
    // reply to a locally deleted yet unsent message, which was available server-side
    return false;
  }
  if (old_info.message_id_.is_valid_scheduled() && old_info.message_id_.is_scheduled_server() &&
      new_info.message_id_.is_valid_scheduled() && new_info.message_id_.is_scheduled_server() &&
      old_info.message_id_.get_scheduled_server_message_id() ==
          new_info.message_id_.get_scheduled_server_message_id()) {
    // schedule date change
    return false;
  }
  if (is_yet_unsent && old_top_thread_message_id == new_info.message_id_ && new_info.dialog_id_ == DialogId()) {
    // move of reply to the top thread message after deletion of the replied message
    return false;
  }
  return true;
}

void RepliedMessageInfo::add_dependencies(Dependencies &dependencies, bool is_bot) const {
  dependencies.add_dialog_and_dependencies(dialog_id_);
  origin_.add_dependencies(dependencies);
  add_formatted_text_dependencies(dependencies, &quote_);
  if (content_ != nullptr) {
    add_message_content_dependencies(dependencies, content_.get(), is_bot);
  }
}

td_api::object_ptr<td_api::messageReplyToMessage> RepliedMessageInfo::get_message_reply_to_message_object(
    Td *td, DialogId dialog_id) const {
  if (dialog_id_.is_valid()) {
    dialog_id = dialog_id_;
  } else {
    CHECK(dialog_id.is_valid());
  }
  auto chat_id = td->messages_manager_->get_chat_id_object(dialog_id, "messageReplyToMessage");

  td_api::object_ptr<td_api::formattedText> quote;
  if (!quote_.text.empty()) {
    quote = get_formatted_text_object(quote_, true, -1);
  }

  td_api::object_ptr<td_api::MessageOrigin> origin;
  if (!origin_.is_empty()) {
    origin = origin_.get_message_origin_object(td);
    CHECK(origin != nullptr);
    CHECK(origin->get_id() != td_api::messageOriginChannel::ID);
  }

  td_api::object_ptr<td_api::MessageContent> content;
  if (content_ != nullptr) {
    content = get_message_content_object(content_.get(), td, dialog_id, 0, false, true, -1, false);
    if (content->get_id() == td_api::messageUnsupported::ID) {
      content = nullptr;
    }
  }

  return td_api::make_object<td_api::messageReplyToMessage>(chat_id, message_id_.get(), std::move(quote),
                                                            is_quote_manual_, std::move(origin), origin_date_,
                                                            std::move(content));
}

MessageId RepliedMessageInfo::get_same_chat_reply_to_message_id() const {
  return is_same_chat_reply() ? message_id_ : MessageId();
}

MessageFullId RepliedMessageInfo::get_reply_message_full_id(DialogId owner_dialog_id) const {
  if (!message_id_.is_valid() && !message_id_.is_valid_scheduled()) {
    return {};
  }
  return {dialog_id_.is_valid() ? dialog_id_ : owner_dialog_id, message_id_};
}

bool operator==(const RepliedMessageInfo &lhs, const RepliedMessageInfo &rhs) {
  if (!(lhs.message_id_ == rhs.message_id_ && lhs.dialog_id_ == rhs.dialog_id_ &&
        lhs.origin_date_ == rhs.origin_date_ && lhs.origin_ == rhs.origin_ && lhs.quote_ == rhs.quote_ &&
        lhs.is_quote_manual_ == rhs.is_quote_manual_)) {
    return false;
  }
  bool need_update = false;
  bool is_content_changed = false;
  compare_message_contents(nullptr, lhs.content_.get(), rhs.content_.get(), is_content_changed, need_update);
  if (need_update || is_content_changed) {
    return false;
  }
  return true;
}

bool operator!=(const RepliedMessageInfo &lhs, const RepliedMessageInfo &rhs) {
  return !(lhs == rhs);
}

StringBuilder &operator<<(StringBuilder &string_builder, const RepliedMessageInfo &info) {
  string_builder << "reply to " << info.message_id_;
  if (info.dialog_id_ != DialogId()) {
    string_builder << " in " << info.dialog_id_;
  }
  if (info.origin_date_ != 0) {
    string_builder << " sent at " << info.origin_date_ << " by " << info.origin_;
  }
  if (!info.quote_.text.empty()) {
    string_builder << " with " << info.quote_.text.size() << (info.is_quote_manual_ ? " manually" : "")
                   << " quoted bytes";
  }
  if (info.content_ != nullptr) {
    string_builder << " and content of the type " << info.content_->get_type();
  }
  return string_builder;
}

}  // namespace td