#include "config.h"

#include "empathy-roster-view.h"

#include <glib/gi18n-lib.h>

#include <libempathy-gtk/empathy-roster-contact.h>
#include <libempathy-gtk/empathy-roster-group.h>
#include <libempathy-gtk/empathy-ui-utils.h>

G_DEFINE_TYPE (EmpathyRosterView, empathy_roster_view, EGG_TYPE_LIST_BOX)

/* Flashing delay for icons (milliseconds). */
#define FLASH_TIMEOUT 500

enum
{
  PROP_MANAGER = 1,
  PROP_SHOW_OFFLINE,
  PROP_SHOW_GROUPS,
  PROP_EMPTY,
  N_PROPS
};

enum
{
  SIG_INDIVIDUAL_ACTIVATED,
  SIG_POPUP_INDIVIDUAL_MENU,
  SIG_EVENT_ACTIVATED,
  SIG_INDIVIDUAL_TOOLTIP,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

#define NO_GROUP "X-no-group"
#define UNGROUPED _("Ungrouped")
#define TOP_GROUP _("Top Contacts")

struct _EmpathyRosterViewPriv
{
  EmpathyIndividualManager *manager;

  /* FolksIndividual (borrowed) -> GHashTable (
   * (gchar * group_name) -> EmpathyRosterContact (borrowed))
   *
   * When not using groups, this hash just have one element mapped
   * from the special NO_GROUP key. We could use it as a set but
   * I prefer to stay coherent in the way this hash is managed.
   */
  GHashTable *roster_contacts;
  /* (gchar *group_name) -> EmpathyRosterGroup (borrowed) */
  GHashTable *roster_groups;
  /* Hash of the EmpathyRosterContact currently displayed */
  GHashTable *displayed_contacts;

  guint last_event_id;
  /* queue of (Event *). The most recent events are in the head of the queue
   * so we always display the icon of the oldest one. */
  GQueue *events;
  guint flash_id;
  gboolean display_flash_event;

  gboolean show_offline;
  gboolean show_groups;
  gboolean empty;

  EmpathyLiveSearch *search;
};

typedef struct
{
  guint id;
  FolksIndividual *individual;
  gchar *icon;
  gpointer user_data;
} Event;

static Event *
event_new (guint id,
    FolksIndividual *individual,
    const gchar *icon,
    gpointer user_data)
{
  Event *event = g_slice_new (Event);

  event->id = id;
  event->individual = g_object_ref (individual);
  event->icon = g_strdup (icon);
  event->user_data = user_data;
  return event;
}

static void
event_free (gpointer data)
{
  Event *event = data;
  g_object_unref (event->individual);
  g_free (event->icon);

  g_slice_free (Event, event);
}

static void
empathy_roster_view_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);

  switch (property_id)
    {
      case PROP_MANAGER:
        g_value_set_object (value, self->priv->manager);
        break;
      case PROP_SHOW_OFFLINE:
        g_value_set_boolean (value, self->priv->show_offline);
        break;
      case PROP_SHOW_GROUPS:
        g_value_set_boolean (value, self->priv->show_groups);
        break;
      case PROP_EMPTY:
        g_value_set_boolean (value, self->priv->empty);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
empathy_roster_view_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);

  switch (property_id)
    {
      case PROP_MANAGER:
        g_assert (self->priv->manager == NULL); /* construct only */
        self->priv->manager = g_value_dup_object (value);
        break;
      case PROP_SHOW_OFFLINE:
        empathy_roster_view_show_offline (self, g_value_get_boolean (value));
        break;
      case PROP_SHOW_GROUPS:
        empathy_roster_view_show_groups (self, g_value_get_boolean (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
roster_contact_changed_cb (GtkWidget *child,
    GParamSpec *spec,
    EmpathyRosterView *self)
{
  egg_list_box_child_changed (EGG_LIST_BOX (self), child);
}

static GtkWidget *
add_roster_contact (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group)
{
  GtkWidget *contact;

  contact = empathy_roster_contact_new (individual, group);

  /* Need to refilter if online is changed */
  g_signal_connect (contact, "notify::online",
      G_CALLBACK (roster_contact_changed_cb), self);

  /* Need to resort if alias is changed */
  g_signal_connect (contact, "notify::alias",
      G_CALLBACK (roster_contact_changed_cb), self);

  gtk_widget_show (contact);
  gtk_container_add (GTK_CONTAINER (self), contact);

  return contact;
}

static void
group_expanded_cb (EmpathyRosterGroup *group,
    GParamSpec *spec,
    EmpathyRosterView *self)
{
  GList *widgets, *l;

  widgets = empathy_roster_group_get_widgets (group);
  for (l = widgets; l != NULL; l = g_list_next (l))
    {
      egg_list_box_child_changed (EGG_LIST_BOX (self), l->data);
    }

  g_list_free (widgets);
}

static EmpathyRosterGroup *
lookup_roster_group (EmpathyRosterView *self,
    const gchar *group)
{
  return g_hash_table_lookup (self->priv->roster_groups, group);
}

static EmpathyRosterGroup *
ensure_roster_group (EmpathyRosterView *self,
    const gchar *group)
{
  GtkWidget *roster_group;

  roster_group = (GtkWidget *) lookup_roster_group (self, group);
  if (roster_group != NULL)
    return EMPATHY_ROSTER_GROUP (roster_group);

  if (!tp_strdiff (group, TOP_GROUP))
    roster_group = empathy_roster_group_new (group, "emblem-favorite-symbolic");
  else
    roster_group = empathy_roster_group_new (group, NULL);

  g_signal_connect (roster_group, "notify::expanded",
      G_CALLBACK (group_expanded_cb), self);

  gtk_widget_show (roster_group);
  gtk_container_add (GTK_CONTAINER (self), roster_group);

  g_hash_table_insert (self->priv->roster_groups, g_strdup (group),
      roster_group);

  return EMPATHY_ROSTER_GROUP (roster_group);
}

static void
update_group_widgets (EmpathyRosterView *self,
    EmpathyRosterGroup *group,
    EmpathyRosterContact *contact,
    gboolean add)
{
  guint old_count, count;

  old_count = empathy_roster_group_get_widgets_count (group);

  if (add)
    count = empathy_roster_group_add_widget (group, GTK_WIDGET (contact));
  else
    count = empathy_roster_group_remove_widget (group, GTK_WIDGET (contact));

  if (count != old_count)
    egg_list_box_child_changed (EGG_LIST_BOX (self), GTK_WIDGET (group));
}

static void
add_to_group (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group)
{
  GtkWidget *contact;
  GHashTable *contacts;
  EmpathyRosterGroup *roster_group = NULL;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  if (tp_strdiff (group, NO_GROUP))
    roster_group = ensure_roster_group (self, group);

  contact = add_roster_contact (self, individual, group);
  g_hash_table_insert (contacts, g_strdup (group), contact);

  if (roster_group != NULL)
    {
      update_group_widgets (self, roster_group,
          EMPATHY_ROSTER_CONTACT (contact), TRUE);
    }
}

static void
individual_added (EmpathyRosterView *self,
    FolksIndividual *individual)
{
  GHashTable *contacts;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts != NULL)
    return;

  contacts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_hash_table_insert (self->priv->roster_contacts, individual, contacts);

  if (!self->priv->show_groups)
    {
      add_to_group (self, individual, NO_GROUP);
    }
  else
    {
      GeeSet *groups;
      GList *tops;

      tops = empathy_individual_manager_get_top_individuals (self->priv->manager);

      if (folks_favourite_details_get_is_favourite (
            FOLKS_FAVOURITE_DETAILS (individual)) || 
          g_list_index (tops, individual) != -1)
        {
          add_to_group (self, individual, TOP_GROUP);
        }

      groups = folks_group_details_get_groups (
          FOLKS_GROUP_DETAILS (individual));

      if (gee_collection_get_size (GEE_COLLECTION (groups)) > 0)
        {
          GeeIterator *iter = gee_iterable_iterator (GEE_ITERABLE (groups));

          while (iter != NULL && gee_iterator_next (iter))
            {
              gchar *group = gee_iterator_get (iter);

              add_to_group (self, individual, group);

              g_free (group);
            }

          g_clear_object (&iter);
        }
      else
        {
          /* No group, adds to Ungrouped */
          add_to_group (self, individual, UNGROUPED);
        }
    }
}

static void
set_event_icon_on_individual (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *icon)
{
  GHashTable *contacts;
  GHashTableIter iter;
  gpointer v;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  g_hash_table_iter_init (&iter, contacts);
  while (g_hash_table_iter_next (&iter, NULL, &v))
    {
      EmpathyRosterContact *contact =v;

      empathy_roster_contact_set_event_icon (contact, icon);
    }
}

static void
flash_event (Event *event,
    EmpathyRosterView *self)
{
  set_event_icon_on_individual (self, event->individual, event->icon);
}

static void
unflash_event (Event *event,
    EmpathyRosterView *self)
{
  set_event_icon_on_individual (self, event->individual, NULL);
}

static gboolean
flash_cb (gpointer data)
{
  EmpathyRosterView *self = data;

  if (self->priv->display_flash_event)
    {
      g_queue_foreach (self->priv->events, (GFunc) flash_event, self);
      self->priv->display_flash_event = FALSE;
    }
  else
    {
      g_queue_foreach (self->priv->events, (GFunc) unflash_event, self);
      self->priv->display_flash_event = TRUE;
    }

  return TRUE;
}

static void
start_flashing (EmpathyRosterView *self)
{
  if (self->priv->flash_id != 0)
    return;

  self->priv->display_flash_event = TRUE;

  self->priv->flash_id = g_timeout_add (FLASH_TIMEOUT,
      flash_cb, self);
}

static void
stop_flashing (EmpathyRosterView *self)
{
  if (self->priv->flash_id == 0)
    return;

  g_source_remove (self->priv->flash_id);
  self->priv->flash_id = 0;
}

static void
remove_event (EmpathyRosterView *self,
    Event *event)
{
  unflash_event (event, self);
  g_queue_remove (self->priv->events, event);

  if (g_queue_get_length (self->priv->events) == 0)
    {
      stop_flashing (self);
    }
}

static void
remove_all_individual_event (EmpathyRosterView *self,
    FolksIndividual *individual)
{
  GList *l;

  for (l = g_queue_peek_head_link (self->priv->events); l != NULL;
      l = g_list_next (l))
    {
      Event *event = l->data;

      if (event->individual == individual)
        {
          remove_event (self, event);
          return;
        }
    }
}

static void
individual_removed (EmpathyRosterView *self,
    FolksIndividual *individual)
{
  GHashTable *contacts;
  GHashTableIter iter;
  gpointer key, value;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  remove_all_individual_event (self, individual);

  g_hash_table_iter_init (&iter, contacts);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const gchar *group_name = key;
      GtkWidget *contact = value;
      EmpathyRosterGroup *group;

      group = lookup_roster_group (self, group_name);
      if (group != NULL)
        {
          update_group_widgets (self, group,
              EMPATHY_ROSTER_CONTACT (contact), FALSE);
        }

      gtk_container_remove (GTK_CONTAINER (self), contact);
    }

  g_hash_table_remove (self->priv->roster_contacts, individual);
}

static void
members_changed_cb (EmpathyIndividualManager *manager,
    const gchar *message,
    GList *added,
    GList *removed,
    TpChannelGroupChangeReason reason,
    EmpathyRosterView *self)
{
  GList *l;

  for (l = added; l != NULL; l = g_list_next (l))
    {
      FolksIndividual *individual = l->data;

      individual_added (self, individual);
    }

  for (l = removed; l != NULL; l = g_list_next (l))
    {
      FolksIndividual *individual = l->data;

      individual_removed (self, individual);
    }
}

static gint
compare_roster_contacts_by_alias (EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  FolksIndividual *ind_a, *ind_b;
  const gchar *alias_a, *alias_b;

  ind_a = empathy_roster_contact_get_individual (a);
  ind_b = empathy_roster_contact_get_individual (b);

  alias_a = folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (ind_a));
  alias_b = folks_alias_details_get_alias (FOLKS_ALIAS_DETAILS (ind_b));

  return g_ascii_strcasecmp (alias_a, alias_b);
}

static gboolean
contact_is_favourite (EmpathyRosterContact *contact)
{
  FolksIndividual *individual;

  individual = empathy_roster_contact_get_individual (contact);

  return folks_favourite_details_get_is_favourite (
      FOLKS_FAVOURITE_DETAILS (individual));
}

static gboolean
contact_in_top (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  FolksIndividual *individual;
  GList *tops;

  if (contact_is_favourite (contact))
    return TRUE;

  individual = empathy_roster_contact_get_individual (contact);

  tops = empathy_individual_manager_get_top_individuals (self->priv->manager);

  if (g_list_index (tops, individual) != -1)
    return TRUE;

  return FALSE;
}

static gint
compare_roster_contacts_no_group (EmpathyRosterView *self,
    EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  gboolean top_a, top_b;

  top_a = contact_in_top (self, a);
  top_b = contact_in_top (self, b);

  if (top_a == top_b)
    /* Both contacts are in the top of the roster (or not). Sort them
     * alphabetically */
    return compare_roster_contacts_by_alias (a, b);
  else if (top_a)
    return -1;
  else
    return 1;
}

static gint
compare_group_names (const gchar *group_a,
    const gchar *group_b)
{
  if (!tp_strdiff (group_a, TOP_GROUP))
    return -1;

  if (!tp_strdiff (group_b, TOP_GROUP))
    return 1;

  return g_ascii_strcasecmp (group_a, group_b);
}

static gint
compare_roster_contacts_with_groups (EmpathyRosterView *self,
    EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  const gchar *group_a, *group_b;

  group_a = empathy_roster_contact_get_group (a);
  group_b = empathy_roster_contact_get_group (b);

  if (!tp_strdiff (group_a, group_b))
    /* Same group, compare the contacts */
    return compare_roster_contacts_by_alias (a, b);

  /* Sort by group */
  return compare_group_names (group_a, group_b);
}

static gint
compare_roster_contacts (EmpathyRosterView *self,
    EmpathyRosterContact *a,
    EmpathyRosterContact *b)
{
  if (!self->priv->show_groups)
    return compare_roster_contacts_no_group (self, a, b);
  else
    return compare_roster_contacts_with_groups (self, a, b);
}

static gint
compare_roster_groups (EmpathyRosterGroup *a,
    EmpathyRosterGroup *b)
{
  const gchar *name_a, *name_b;

  name_a = empathy_roster_group_get_name (a);
  name_b = empathy_roster_group_get_name (b);

  return compare_group_names (name_a, name_b);
}

static gint
compare_contact_group (EmpathyRosterContact *contact,
    EmpathyRosterGroup *group)
{
  const char *contact_group, *group_name;

  contact_group = empathy_roster_contact_get_group (contact);
  group_name = empathy_roster_group_get_name (group);

  if (!tp_strdiff (contact_group, group_name))
    /* @contact is in @group, @group has to be displayed first */
    return 1;

  /* @contact is in a different group, sort by group name */
  return compare_group_names (contact_group, group_name);
}

static gint
roster_view_sort (gconstpointer a,
    gconstpointer b,
    gpointer user_data)
{
  EmpathyRosterView *self = user_data;

  if (EMPATHY_IS_ROSTER_CONTACT (a) && EMPATHY_IS_ROSTER_CONTACT (b))
    return compare_roster_contacts (self, EMPATHY_ROSTER_CONTACT (a),
        EMPATHY_ROSTER_CONTACT (b));
  else if (EMPATHY_IS_ROSTER_GROUP (a) && EMPATHY_IS_ROSTER_GROUP (b))
    return compare_roster_groups (EMPATHY_ROSTER_GROUP (a),
        EMPATHY_ROSTER_GROUP (b));
  else if (EMPATHY_IS_ROSTER_CONTACT (a) && EMPATHY_IS_ROSTER_GROUP (b))
    return compare_contact_group (EMPATHY_ROSTER_CONTACT (a),
        EMPATHY_ROSTER_GROUP (b));
  else if (EMPATHY_IS_ROSTER_GROUP (a) && EMPATHY_IS_ROSTER_CONTACT (b))
    return -1 * compare_contact_group (EMPATHY_ROSTER_CONTACT (b),
        EMPATHY_ROSTER_GROUP (a));

  g_return_val_if_reached (0);
}

static void
update_separator (GtkWidget **separator,
    GtkWidget *child,
    GtkWidget *before,
    gpointer user_data)
{
  if (before == NULL)
    {
      /* No separator before the first row */
      g_clear_object (separator);
      return;
    }

  if (*separator != NULL)
    return;

  *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  g_object_ref_sink (*separator);
}

static gboolean
is_searching (EmpathyRosterView *self)
{
  if (self->priv->search == NULL)
    return FALSE;

  return gtk_widget_get_visible (GTK_WIDGET (self->priv->search));
}

static void
update_empty (EmpathyRosterView *self,
    gboolean empty)
{
  if (self->priv->empty == empty)
    return;

  self->priv->empty = empty;
  g_object_notify (G_OBJECT (self), "empty");
}

static void
add_to_displayed (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  FolksIndividual *individual;
  GHashTable *contacts;
  GHashTableIter iter;
  gpointer k;

  if (g_hash_table_lookup (self->priv->displayed_contacts, contact) != NULL)
    return;

  g_hash_table_add (self->priv->displayed_contacts, contact);
  update_empty (self, FALSE);

  /* Groups of this contact may now be displayed if we just displays the first
   * child in this group. */
  individual = empathy_roster_contact_get_individual (contact);
  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  g_hash_table_iter_init (&iter, contacts);
  while (g_hash_table_iter_next (&iter, &k, NULL))
    {
      const gchar *group_name = k;
      GtkWidget *group;

      group = g_hash_table_lookup (self->priv->roster_groups, group_name);
      if (group == NULL)
        continue;

      egg_list_box_child_changed (EGG_LIST_BOX (self), group);
    }
}

static void
remove_from_displayed (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  g_hash_table_remove (self->priv->displayed_contacts, contact);

  if (g_hash_table_size (self->priv->displayed_contacts) == 0)
    update_empty (self, TRUE);
}

/**
 * check if @contact should be displayed according to @self's current status
 * and without consideration for the state of @contact's groups.
 */
static gboolean
contact_should_be_displayed (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  if (is_searching (self))
    {
      FolksIndividual *individual;

      individual = empathy_roster_contact_get_individual (contact);

      return empathy_individual_match_string (individual,
          empathy_live_search_get_text (self->priv->search),
          empathy_live_search_get_words (self->priv->search));
    }
  else
    {
      if (self->priv->show_offline)
        {
          return TRUE;
        }
      else if (!self->priv->show_groups &&
          contact_is_favourite (contact))
        {
          /* Always display favourite contacts in non-group mode. In the group
           * mode we'll display only the one in the 'top' section. */
          return TRUE;
        }
      else
        {
          return empathy_roster_contact_is_online (contact);
        }
    }
}

static gboolean
filter_contact (EmpathyRosterView *self,
    EmpathyRosterContact *contact)
{
  gboolean displayed;

  displayed = contact_should_be_displayed (self, contact);

  if (self->priv->show_groups)
    {
      const gchar *group_name;
      EmpathyRosterGroup *group;

      group_name = empathy_roster_contact_get_group (contact);
      group = lookup_roster_group (self, group_name);

      if (!tp_strdiff (group_name, TOP_GROUP) &&
          contact_is_favourite (contact))
        displayed = TRUE;

      if (group != NULL)
        {
          /* When searching, always display even if the group is closed */
          if (!is_searching (self) &&
              !gtk_expander_get_expanded (GTK_EXPANDER (group)))
            displayed = FALSE;
        }
    }

  if (displayed)
    {
      add_to_displayed (self, contact);
    }
  else
    {
      remove_from_displayed (self, contact);
    }

  return displayed;
}

static gboolean
filter_group (EmpathyRosterView *self,
    EmpathyRosterGroup *group)
{
  GList *widgets, *l;

  /* Display the group if it contains at least one displayed contact */
  widgets = empathy_roster_group_get_widgets (group);
  for (l = widgets; l != NULL; l = g_list_next (l))
    {
      EmpathyRosterContact *contact = l->data;

      if (contact_should_be_displayed (self, contact))
        return TRUE;
    }

  return FALSE;
}

static gboolean
filter_list (GtkWidget *child,
    gpointer user_data)
{
  EmpathyRosterView *self = user_data;

  if (EMPATHY_IS_ROSTER_CONTACT (child))
    return filter_contact (self, EMPATHY_ROSTER_CONTACT (child));

  else if (EMPATHY_IS_ROSTER_GROUP (child))
    return filter_group (self, EMPATHY_ROSTER_GROUP (child));

  g_return_val_if_reached (FALSE);
}

/* @list: GList of EmpathyRosterContact
 *
 * Returns: %TRUE if @list contains an EmpathyRosterContact associated with
 * @individual */
static gboolean
individual_in_list (FolksIndividual *individual,
    GList *list)
{
  GList *l;

  for (l = list; l != NULL; l = g_list_next (l))
    {
      EmpathyRosterContact *contact = l->data;

      if (empathy_roster_contact_get_individual (contact) == individual)
        return TRUE;
    }

  return FALSE;
}

static void
populate_view (EmpathyRosterView *self)
{
  GList *individuals, *l;

  individuals = empathy_individual_manager_get_members (self->priv->manager);
  for (l = individuals; l != NULL; l = g_list_next (l))
    {
      FolksIndividual *individual = l->data;

      individual_added (self, individual);
    }

  g_list_free (individuals);
}

static void
remove_from_group (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *group)
{
  GHashTable *contacts;
  GtkWidget *contact;
  EmpathyRosterGroup *roster_group;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  contact = g_hash_table_lookup (contacts, group);
  if (contact == NULL)
    return;

  g_hash_table_remove (contacts, group);

  if (g_hash_table_size (contacts) == 0)
    {
      add_to_group (self, individual, UNGROUPED);
    }

  roster_group = lookup_roster_group (self, group);

  if (roster_group != NULL)
    {
      update_group_widgets (self, roster_group,
          EMPATHY_ROSTER_CONTACT (contact), FALSE);
    }

  gtk_container_remove (GTK_CONTAINER (self), contact);
}

static void
update_top_contacts (EmpathyRosterView *self)
{
  GList *tops, *l;
  GList *to_add = NULL, *to_remove = NULL;
  EmpathyRosterGroup *group;

  if (!self->priv->show_groups)
    {
      egg_list_box_resort (EGG_LIST_BOX (self));
      return;
    }

  tops = empathy_individual_manager_get_top_individuals (self->priv->manager);

  group = g_hash_table_lookup (self->priv->roster_groups, TOP_GROUP);
  if (group == NULL)
    {
      to_add = g_list_copy (tops);
    }
  else
    {
      GList *contacts;

      contacts = empathy_roster_group_get_widgets (group);

      /* Check which EmpathyRosterContact have to be removed */
      for (l = contacts; l != NULL; l = g_list_next (l))
        {
          EmpathyRosterContact *contact = l->data;
          FolksIndividual *individual;

          if (contact_is_favourite (contact))
            continue;

          individual = empathy_roster_contact_get_individual (contact);

          if (g_list_find (tops, individual) == NULL)
            to_remove = g_list_prepend (to_remove, individual);
        }

      /* Check which EmpathyRosterContact have to be added */
      for (l = tops; l != NULL; l = g_list_next (l))
        {
          FolksIndividual *individual = l->data;

          if (!individual_in_list (individual, contacts))
            to_add = g_list_prepend (to_add, individual);
        }
    }

  for (l = to_add; l != NULL; l = g_list_next (l))
    add_to_group (self, l->data, TOP_GROUP);

  for (l = to_remove; l != NULL; l = g_list_next (l))
    remove_from_group (self, l->data, TOP_GROUP);

  g_list_free (to_add);
  g_list_free (to_remove);
}

static void
groups_changed_cb (EmpathyIndividualManager *manager,
    FolksIndividual *individual,
    gchar *group,
    gboolean is_member,
    EmpathyRosterView *self)
{
  if (!self->priv->show_groups)
    return;

  if (is_member)
    {
      add_to_group (self, individual, group);
    }
  else
    {
      remove_from_group (self, individual, group);
    }
}

static void
top_individuals_changed_cb (EmpathyIndividualManager *manager,
    GParamSpec *spec,
    EmpathyRosterView *self)
{
  update_top_contacts (self);
}

static void
favourites_changed_cb (EmpathyIndividualManager *manager,
    FolksIndividual *individual,
    gboolean favourite,
    EmpathyRosterView *self)
{
  GHashTable *contacts;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return;

  if (self->priv->show_groups)
    {
      if (favourite)
        add_to_group (self, individual, TOP_GROUP);
      else
        remove_from_group (self, individual, TOP_GROUP);
    }
  else
    {
      GtkWidget *contact;

      contact = g_hash_table_lookup (contacts, NO_GROUP);
      if (contact == NULL)
        return;

      egg_list_box_child_changed (EGG_LIST_BOX (self), contact);
    }
}

static void
empathy_roster_view_constructed (GObject *object)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_view_parent_class)->constructed;

  if (chain_up != NULL)
    chain_up (object);

  g_assert (EMPATHY_IS_INDIVIDUAL_MANAGER (self->priv->manager));

  populate_view (self);

  tp_g_signal_connect_object (self->priv->manager, "members-changed",
      G_CALLBACK (members_changed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->manager, "groups-changed",
      G_CALLBACK (groups_changed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->manager, "notify::top-individuals",
      G_CALLBACK (top_individuals_changed_cb), self, 0);
  tp_g_signal_connect_object (self->priv->manager, "notify::favourites-changed",
      G_CALLBACK (favourites_changed_cb), self, 0);

  egg_list_box_set_sort_func (EGG_LIST_BOX (self),
      roster_view_sort, self, NULL);

  egg_list_box_set_separator_funcs (EGG_LIST_BOX (self), update_separator,
      self, NULL);

  egg_list_box_set_filter_func (EGG_LIST_BOX (self), filter_list, self, NULL);

  egg_list_box_set_activate_on_single_click (EGG_LIST_BOX (self), FALSE);
}

static void
empathy_roster_view_dispose (GObject *object)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_view_parent_class)->dispose;

  stop_flashing (self);

  empathy_roster_view_set_live_search (self, NULL);
  g_clear_object (&self->priv->manager);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_roster_view_finalize (GObject *object)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (object);
  void (*chain_up) (GObject *) =
      ((GObjectClass *) empathy_roster_view_parent_class)->finalize;

  g_hash_table_unref (self->priv->roster_contacts);
  g_hash_table_unref (self->priv->roster_groups);
  g_hash_table_unref (self->priv->displayed_contacts);
  g_queue_free_full (self->priv->events, event_free);

  if (chain_up != NULL)
    chain_up (object);
}

static void
empathy_roster_view_child_activated (EggListBox *box,
    GtkWidget *child)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (box);
  EmpathyRosterContact *contact;
  FolksIndividual *individual;
  GList *l;

  if (!EMPATHY_IS_ROSTER_CONTACT (child))
    return;

  contact = EMPATHY_ROSTER_CONTACT (child);
  individual = empathy_roster_contact_get_individual (contact);

  /* Activate the oldest event associated with this contact, if any */
  for (l = g_queue_peek_tail_link (self->priv->events); l != NULL;
      l = g_list_previous (l))
    {
      Event *event = l->data;

      if (event->individual == individual)
        {
          g_signal_emit (box, signals[SIG_EVENT_ACTIVATED], 0, individual,
              event->user_data);
          return;
        }
    }

  g_signal_emit (box, signals[SIG_INDIVIDUAL_ACTIVATED], 0, individual);
}

static void
fire_popup_individual_menu (EmpathyRosterView *self,
    GtkWidget *child,
    guint button,
    guint time)
{
  EmpathyRosterContact *contact;
  FolksIndividual *individual;

  if (!EMPATHY_IS_ROSTER_CONTACT (child))
    return;

  contact = EMPATHY_ROSTER_CONTACT (child);
  individual = empathy_roster_contact_get_individual (contact);

  g_signal_emit (self, signals[SIG_POPUP_INDIVIDUAL_MENU], 0,
      individual, button, time);
}

static gboolean
empathy_roster_view_button_press_event (GtkWidget *widget,
    GdkEventButton *event)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (widget);
  gboolean (*chain_up) (GtkWidget *, GdkEventButton *) =
      ((GtkWidgetClass *) empathy_roster_view_parent_class)->button_press_event;

  if (event->button == 3)
    {
      GtkWidget *child;

      child = egg_list_box_get_child_at_y (EGG_LIST_BOX (self), event->y);

      if (child != NULL)
        {
          egg_list_box_select_child (EGG_LIST_BOX (self), child);

          fire_popup_individual_menu (self, child, event->button, event->time);
        }
    }

  return chain_up (widget, event);
}

static gboolean
empathy_roster_view_key_press_event (GtkWidget *widget,
    GdkEventKey *event)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (widget);
  gboolean (*chain_up) (GtkWidget *, GdkEventKey *) =
      ((GtkWidgetClass *) empathy_roster_view_parent_class)->key_press_event;

  if (event->keyval == GDK_KEY_Menu)
    {
      GtkWidget *child;

      child = egg_list_box_get_selected_child (EGG_LIST_BOX (self));

      if (child != NULL)
        fire_popup_individual_menu (self, child, 0, event->time);
    }

  return chain_up (widget, event);
}

static gboolean
empathy_roster_view_query_tooltip (GtkWidget *widget,
    gint x,
    gint y,
    gboolean keyboard_mode,
    GtkTooltip *tooltip)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (widget);
  GtkWidget *child;
  EmpathyRosterContact *contact;
  FolksIndividual *individual;
  gboolean result;

  child = egg_list_box_get_child_at_y (EGG_LIST_BOX (self), y);
  if (!EMPATHY_IS_ROSTER_CONTACT (child))
    return FALSE;

  contact = EMPATHY_ROSTER_CONTACT (child);
  individual = empathy_roster_contact_get_individual (contact);

  g_signal_emit (self, signals[SIG_INDIVIDUAL_TOOLTIP], 0,
      individual, keyboard_mode, tooltip, &result);

  if (result)
    {
      GtkAllocation allocation;

      gtk_widget_get_allocation (child, &allocation);
      gtk_tooltip_set_tip_area (tooltip, (GdkRectangle *) &allocation);
    }

  return result;
}

static void
empathy_roster_view_remove (GtkContainer *container,
    GtkWidget *widget)
{
  EmpathyRosterView *self = EMPATHY_ROSTER_VIEW (container);
  void (*chain_up) (GtkContainer *, GtkWidget *) =
      ((GtkContainerClass *) empathy_roster_view_parent_class)->remove;

  chain_up (container, widget);

  if (EMPATHY_IS_ROSTER_CONTACT (widget))
    remove_from_displayed (self, (EmpathyRosterContact *) widget);
}

static void
empathy_roster_view_class_init (
    EmpathyRosterViewClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  EggListBoxClass *box_class = EGG_LIST_BOX_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);
  GParamSpec *spec;

  oclass->get_property = empathy_roster_view_get_property;
  oclass->set_property = empathy_roster_view_set_property;
  oclass->constructed = empathy_roster_view_constructed;
  oclass->dispose = empathy_roster_view_dispose;
  oclass->finalize = empathy_roster_view_finalize;

  widget_class->button_press_event = empathy_roster_view_button_press_event;
  widget_class->key_press_event = empathy_roster_view_key_press_event;
  widget_class->query_tooltip = empathy_roster_view_query_tooltip;

  container_class->remove = empathy_roster_view_remove;

  box_class->child_activated = empathy_roster_view_child_activated;

  spec = g_param_spec_object ("manager", "Manager",
      "EmpathyIndividualManager",
      EMPATHY_TYPE_INDIVIDUAL_MANAGER,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_MANAGER, spec);

  spec = g_param_spec_boolean ("show-offline", "Show Offline",
      "Show offline contacts",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_SHOW_OFFLINE, spec);

  spec = g_param_spec_boolean ("show-groups", "Show Groups",
      "Show groups",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_SHOW_GROUPS, spec);

  spec = g_param_spec_boolean ("empty", "Empty",
      "Is the view currently empty?",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_EMPTY, spec);

  signals[SIG_INDIVIDUAL_ACTIVATED] = g_signal_new ("individual-activated",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      1, FOLKS_TYPE_INDIVIDUAL);

  signals[SIG_POPUP_INDIVIDUAL_MENU] = g_signal_new ("popup-individual-menu",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      3, FOLKS_TYPE_INDIVIDUAL, G_TYPE_UINT, G_TYPE_UINT);

  signals[SIG_EVENT_ACTIVATED] = g_signal_new ("event-activated",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE,
      2, FOLKS_TYPE_INDIVIDUAL, G_TYPE_POINTER);

  signals[SIG_INDIVIDUAL_TOOLTIP] = g_signal_new ("individual-tooltip",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, g_signal_accumulator_true_handled, NULL, NULL,
      G_TYPE_BOOLEAN,
      3, FOLKS_TYPE_INDIVIDUAL, G_TYPE_BOOLEAN, GTK_TYPE_TOOLTIP);

  g_type_class_add_private (klass, sizeof (EmpathyRosterViewPriv));
}

static void
empathy_roster_view_init (EmpathyRosterView *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_ROSTER_VIEW, EmpathyRosterViewPriv);

  self->priv->roster_contacts = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) g_hash_table_unref);
  self->priv->roster_groups = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, NULL);
  self->priv->displayed_contacts = g_hash_table_new (NULL, NULL);

  self->priv->events = g_queue_new ();

  self->priv->empty = TRUE;
}

GtkWidget *
empathy_roster_view_new (EmpathyIndividualManager *manager)
{
  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_MANAGER (manager), NULL);

  return g_object_new (EMPATHY_TYPE_ROSTER_VIEW,
      "manager", manager,
      NULL);
}

EmpathyIndividualManager *
empathy_roster_view_get_manager (EmpathyRosterView *self)
{
  return self->priv->manager;
}

void
empathy_roster_view_show_offline (EmpathyRosterView *self,
    gboolean show)
{
  if (self->priv->show_offline == show)
    return;

  self->priv->show_offline = show;
  egg_list_box_refilter (EGG_LIST_BOX (self));

  g_object_notify (G_OBJECT (self), "show-offline");
}

static void
clear_view (EmpathyRosterView *self)
{
  gtk_container_foreach (GTK_CONTAINER (self),
      (GtkCallback) gtk_widget_destroy, NULL);

  g_hash_table_remove_all (self->priv->roster_contacts);
  g_hash_table_remove_all (self->priv->roster_groups);
  g_hash_table_remove_all (self->priv->displayed_contacts);
}

void
empathy_roster_view_show_groups (EmpathyRosterView *self,
    gboolean show)
{
  if (self->priv->show_groups == show)
    return;

  self->priv->show_groups = show;

  /* TODO: block sort/filter? */
  clear_view (self);
  populate_view (self);

  g_object_notify (G_OBJECT (self), "show-groups");
}

static void
select_first_contact (EmpathyRosterView *self)
{
  GList *children, *l;

  children = gtk_container_get_children (GTK_CONTAINER (self));
  for (l = children; l != NULL; l = g_list_next (l))
    {
      GtkWidget *child = l->data;

      if (!gtk_widget_get_child_visible (child))
        continue;

      if (!EMPATHY_IS_ROSTER_CONTACT (child))
        continue;

      egg_list_box_select_child (EGG_LIST_BOX (self), child);
      break;
    }

  g_list_free (children);
}

static void
search_text_notify_cb (EmpathyLiveSearch *search,
    GParamSpec *pspec,
    EmpathyRosterView *self)
{
  egg_list_box_refilter (EGG_LIST_BOX (self));

  select_first_contact (self);
}

static void
search_activate_cb (GtkWidget *search,
  EmpathyRosterView *self)
{
  EggListBox *box = EGG_LIST_BOX (self);
  GtkWidget *child;

  child = egg_list_box_get_selected_child (box);
  if (child == NULL)
    return;

  empathy_roster_view_child_activated (box, child);
}

void
empathy_roster_view_set_live_search (EmpathyRosterView *self,
    EmpathyLiveSearch *search)
{
  if (self->priv->search != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->priv->search,
          search_text_notify_cb, self);
      g_signal_handlers_disconnect_by_func (self->priv->search,
          search_activate_cb, self);

      g_clear_object (&self->priv->search);
    }

  if (search == NULL)
    return;

  self->priv->search = g_object_ref (search);

  g_signal_connect (self->priv->search, "notify::text",
      G_CALLBACK (search_text_notify_cb), self);
  g_signal_connect (self->priv->search, "activate",
      G_CALLBACK (search_activate_cb), self);
}

gboolean
empathy_roster_view_is_empty (EmpathyRosterView *self)
{
  return self->priv->empty;
}

gboolean
empathy_roster_view_is_searching (EmpathyRosterView *self)
{
  return (self->priv->search != NULL &&
      gtk_widget_get_visible (GTK_WIDGET (self->priv->search)));
}

/* Don't use EmpathyEvent as I prefer to keep this object not too specific to
 * Empathy's internals. */
guint
empathy_roster_view_add_event (EmpathyRosterView *self,
    FolksIndividual *individual,
    const gchar *icon,
    gpointer user_data)
{
  GHashTable *contacts;

  contacts = g_hash_table_lookup (self->priv->roster_contacts, individual);
  if (contacts == NULL)
    return 0;

  self->priv->last_event_id++;

  g_queue_push_head (self->priv->events,
      event_new (self->priv->last_event_id, individual, icon, user_data));

  start_flashing (self);

  return self->priv->last_event_id;
}

void
empathy_roster_view_remove_event (EmpathyRosterView *self,
    guint event_id)
{
  GList *l;

  for (l = g_queue_peek_head_link (self->priv->events); l != NULL;
      l = g_list_next (l))
    {
      Event *event = l->data;

      if (event->id == event_id)
        {
          remove_event (self, event);
          return;
        }
    }
}
