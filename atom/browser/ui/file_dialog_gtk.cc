// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/ui/file_dialog.h"

#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

// This conflicts with mate::Converter,
#undef True
#undef False
// and V8.
#undef None

#include "atom/browser/native_window.h"
#include "base/callback.h"
#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ui/libgtk2ui/gtk2_signal.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/views/widget/desktop_aura/x11_desktop_handler.h"

namespace file_dialog {

namespace {

const char kAuraTransientParent[] = "aura-transient-parent";

void SetGtkTransientForAura(GtkWidget* dialog, aura::Window* parent) {
  if (!parent || !parent->GetHost())
    return;

  gtk_widget_realize(dialog);
  GdkWindow* gdk_window = gtk_widget_get_window(dialog);

  // TODO(erg): Check to make sure we're using X11 if wayland or some other
  // display server ever happens. Otherwise, this will crash.
  XSetTransientForHint(GDK_WINDOW_XDISPLAY(gdk_window),
                       GDK_WINDOW_XID(gdk_window),
                       parent->GetHost()->GetAcceleratedWidget());

  // We also set the |parent| as a property of |dialog|, so that we can unlink
  // the two later.
  g_object_set_data(G_OBJECT(dialog), kAuraTransientParent, parent);
}

// Makes sure that .jpg also shows .JPG.
gboolean FileFilterCaseInsensitive(const GtkFileFilterInfo* file_info,
                                   std::string* file_extension) {
  return EndsWith(file_info->filename, *file_extension, false);
}

// Deletes |data| when gtk_file_filter_add_custom() is done with it.
void OnFileFilterDataDestroyed(std::string* file_extension) {
  delete file_extension;
}

class FileChooserDialog {
 public:
  FileChooserDialog(GtkFileChooserAction action,
                    atom::NativeWindow* parent_window,
                    const std::string& title,
                    const base::FilePath& default_path,
                    const Filters& filters)
      : dialog_scope_(new atom::NativeWindow::DialogScope(parent_window)) {
    const char* confirm_text = GTK_STOCK_OK;
    if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
      confirm_text = GTK_STOCK_SAVE;
    else if (action == GTK_FILE_CHOOSER_ACTION_OPEN)
      confirm_text = GTK_STOCK_OPEN;

    dialog_ = gtk_file_chooser_dialog_new(
        title.c_str(),
        NULL,
        action,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        confirm_text, GTK_RESPONSE_ACCEPT,
        NULL);
    if (parent_window) {
      gfx::NativeWindow window = parent_window->GetNativeWindow();
      SetGtkTransientForAura(dialog_, window);
    }

    if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
      gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog_),
                                                     TRUE);
    if (action != GTK_FILE_CHOOSER_ACTION_OPEN)
      gtk_file_chooser_set_create_folders(GTK_FILE_CHOOSER(dialog_), TRUE);

    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);

    if (!default_path.empty()) {
      if (base::DirectoryExists(default_path)) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog_),
                                            default_path.value().c_str());
      } else {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog_),
            default_path.DirName().value().c_str());
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog_),
            default_path.BaseName().value().c_str());
      }
    }

    if (!filters.empty())
      AddFilters(filters);
  }

  virtual ~FileChooserDialog() {
    gtk_widget_destroy(dialog_);
  }

  void RunAsynchronous() {
    g_signal_connect(dialog_, "delete-event",
                     G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    g_signal_connect(dialog_, "response",
                     G_CALLBACK(OnFileDialogResponseThunk), this);
    gtk_widget_show_all(dialog_);

    // We need to call gtk_window_present after making the widgets visible to
    // make sure window gets correctly raised and gets focus.
    int time = views::X11DesktopHandler::get()->wm_user_time_ms();
    gtk_window_present_with_time(GTK_WINDOW(dialog_), time);
  }

  void RunSaveAsynchronous(const SaveDialogCallback& callback) {
    save_callback_ = callback;
    RunAsynchronous();
  }

  void RunOpenAsynchronous(const OpenDialogCallback& callback) {
    open_callback_ = callback;
    RunAsynchronous();
  }

  base::FilePath GetFileName() const {
    gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog_));
    base::FilePath path(filename);
    g_free(filename);
    return path;
  }

  std::vector<base::FilePath> GetFileNames() const {
    std::vector<base::FilePath> paths;
    GSList* filenames = gtk_file_chooser_get_filenames(
        GTK_FILE_CHOOSER(dialog_));
    for (GSList* iter = filenames; iter != NULL; iter = g_slist_next(iter)) {
      base::FilePath path(static_cast<char*>(iter->data));
      g_free(iter->data);
      paths.push_back(path);
    }
    g_slist_free(filenames);
    return paths;
  }

  CHROMEGTK_CALLBACK_1(FileChooserDialog, void, OnFileDialogResponse, int);

  GtkWidget* dialog() const { return dialog_; }

 private:
  void AddFilters(const Filters& filters);

  GtkWidget* dialog_;

  SaveDialogCallback save_callback_;
  OpenDialogCallback open_callback_;

  scoped_ptr<atom::NativeWindow::DialogScope> dialog_scope_;

  DISALLOW_COPY_AND_ASSIGN(FileChooserDialog);
};

void FileChooserDialog::OnFileDialogResponse(GtkWidget* widget, int response) {
  gtk_widget_hide_all(dialog_);

  if (!save_callback_.is_null()) {
    if (response == GTK_RESPONSE_ACCEPT)
      save_callback_.Run(true, GetFileName());
    else
      save_callback_.Run(false, base::FilePath());
  } else if (!open_callback_.is_null()) {
    if (response == GTK_RESPONSE_ACCEPT)
      open_callback_.Run(true, GetFileNames());
    else
      open_callback_.Run(false, std::vector<base::FilePath>());
  }
  delete this;
}

void FileChooserDialog::AddFilters(const Filters& filters) {
  for (size_t i = 0; i < filters.size(); ++i) {
    const Filter& filter = filters[i];
    GtkFileFilter* gtk_filter = gtk_file_filter_new();

    for (size_t j = 0; j < filter.second.size(); ++j) {
      scoped_ptr<std::string> file_extension(
          new std::string("." + filter.second[j]));
      gtk_file_filter_add_custom(
          gtk_filter,
          GTK_FILE_FILTER_FILENAME,
          reinterpret_cast<GtkFileFilterFunc>(FileFilterCaseInsensitive),
          file_extension.release(),
          reinterpret_cast<GDestroyNotify>(OnFileFilterDataDestroyed));
    }

    gtk_file_filter_set_name(gtk_filter, filter.first.c_str());
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog_), gtk_filter);
  }
}

}  // namespace

bool ShowOpenDialog(atom::NativeWindow* parent_window,
                    const std::string& title,
                    const base::FilePath& default_path,
                    const Filters& filters,
                    int properties,
                    std::vector<base::FilePath>* paths) {
  GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
  if (properties & FILE_DIALOG_OPEN_DIRECTORY)
    action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
  FileChooserDialog open_dialog(action, parent_window, title, default_path,
                                filters);
  if (properties & FILE_DIALOG_MULTI_SELECTIONS)
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(open_dialog.dialog()),
                                         TRUE);

  gtk_widget_show_all(open_dialog.dialog());
  int response = gtk_dialog_run(GTK_DIALOG(open_dialog.dialog()));
  if (response == GTK_RESPONSE_ACCEPT) {
    *paths = open_dialog.GetFileNames();
    return true;
  } else {
    return false;
  }
}

void ShowOpenDialog(atom::NativeWindow* parent_window,
                    const std::string& title,
                    const base::FilePath& default_path,
                    const Filters& filters,
                    int properties,
                    const OpenDialogCallback& callback) {
  GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
  if (properties & FILE_DIALOG_OPEN_DIRECTORY)
    action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
  FileChooserDialog* open_dialog = new FileChooserDialog(
      action, parent_window, title, default_path, filters);
  if (properties & FILE_DIALOG_MULTI_SELECTIONS)
    gtk_file_chooser_set_select_multiple(
        GTK_FILE_CHOOSER(open_dialog->dialog()), TRUE);

  open_dialog->RunOpenAsynchronous(callback);
}

bool ShowSaveDialog(atom::NativeWindow* parent_window,
                    const std::string& title,
                    const base::FilePath& default_path,
                    const Filters& filters,
                    base::FilePath* path) {
  FileChooserDialog save_dialog(GTK_FILE_CHOOSER_ACTION_SAVE, parent_window,
                                title, default_path, filters);
  gtk_widget_show_all(save_dialog.dialog());
  int response = gtk_dialog_run(GTK_DIALOG(save_dialog.dialog()));
  if (response == GTK_RESPONSE_ACCEPT) {
    *path = save_dialog.GetFileName();
    return true;
  } else {
    return false;
  }
}

void ShowSaveDialog(atom::NativeWindow* parent_window,
                    const std::string& title,
                    const base::FilePath& default_path,
                    const Filters& filters,
                    const SaveDialogCallback& callback) {
  FileChooserDialog* save_dialog = new FileChooserDialog(
      GTK_FILE_CHOOSER_ACTION_SAVE, parent_window, title, default_path,
      filters);
  save_dialog->RunSaveAsynchronous(callback);
}

}  // namespace file_dialog
