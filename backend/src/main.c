#define _POSIX_C_SOURCE 200809L

#include <microhttpd.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_client.h"
#include "db_sqlite.h"
#include "db_supabase.h"
#include "json_parser.h"
#include "nutrition.h"

#define SERVER_PORT 8080
#define MAX_BODY_SIZE 16384

typedef struct RequestContext
{
    char *body;
    size_t body_size;
    int ready;
} RequestContext;

/* Libère le contexte associé à une requête HTTP. */
static void free_request_context(RequestContext *context)
{
    if (context == NULL)
    {
        return;
    }

    free(context->body);
    free(context);
}

/* Crée une réponse JSON avec les en-têtes CORS. */
static struct MHD_Response *create_json_response(const char *json_text)
{
    struct MHD_Response *response;

    if (json_text == NULL)
    {
        return NULL;
    }

    response = MHD_create_response_from_buffer(strlen(json_text), (void *)json_text, MHD_RESPMEM_MUST_COPY);
    if (response == NULL)
    {
        return NULL;
    }

    MHD_add_response_header(response, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    return response;
}

/* Construit un objet JSON standard avec un statut et une charge utile. */
static char *wrap_payload(cJSON *data_object, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    char *json_text = NULL;

    if (root == NULL)
    {
        cJSON_Delete(data_object);
        return NULL;
    }

    cJSON_AddStringToObject(root, "status", status != NULL ? status : "error");

    if (data_object != NULL)
    {
        cJSON_AddItemToObject(root, "data", data_object);
    }
    else
    {
        cJSON *data = cJSON_CreateObject();
        if (data != NULL && message != NULL)
        {
            cJSON_AddStringToObject(data, "message", message);
        }
        cJSON_AddItemToObject(root, "data", data);
    }

    json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_text;
}

/* Construit une réponse d'erreur standard. */
static char *build_error_response(const char *message)
{
    return wrap_payload(NULL, "error", message);
}

/* Envoie une réponse JSON au client. */
static int queue_json(struct MHD_Connection *connection, unsigned int status_code, const char *json_text)
{
    struct MHD_Response *response = create_json_response(json_text);
    int result;

    if (response == NULL)
    {
        return MHD_NO;
    }

    result = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return result;
}

/* Envoie une erreur JSON au client. */
static int queue_error(struct MHD_Connection *connection, unsigned int status_code, const char *message)
{
    char *json_text = build_error_response(message);
    int result;

    if (json_text == NULL)
    {
        return MHD_NO;
    }

    result = queue_json(connection, status_code, json_text);
    free(json_text);
    return result;
}

/* Lit un entier depuis les paramètres de requête. */
static int query_int(struct MHD_Connection *connection, const char *name, int default_value)
{
    const char *value = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, name);
    return value != NULL ? atoi(value) : default_value;
}

/* Lit un flottant depuis les paramètres de requête. */
static float query_float(struct MHD_Connection *connection, const char *name, float default_value)
{
    const char *value = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, name);
    return value != NULL ? (float)atof(value) : default_value;
}

/* Duplique une chaîne JSON si elle est présente. */
static char *duplicate_json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        return strdup(item->valuestring);
    }

    return NULL;
}

/* Convertit les restrictions en chaîne séparée par des virgules. */
static char *join_restrictions(cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        return strdup(item->valuestring);
    }

    if (!cJSON_IsArray(item))
    {
        return strdup("");
    }

    size_t total = 1;
    int count = cJSON_GetArraySize(item);

    for (int index = 0; index < count; ++index)
    {
        cJSON *entry = cJSON_GetArrayItem(item, index);
        if (cJSON_IsString(entry) && entry->valuestring != NULL)
        {
            total += strlen(entry->valuestring) + 2;
        }
    }

    char *result = (char *)calloc(total, 1);
    if (result == NULL)
    {
        return NULL;
    }

    for (int index = 0; index < count; ++index)
    {
        cJSON *entry = cJSON_GetArrayItem(item, index);
        if (cJSON_IsString(entry) && entry->valuestring != NULL)
        {
            if (result[0] != '\0')
            {
                strcat(result, ", ");
            }
            strcat(result, entry->valuestring);
        }
    }

    return result;
}

/* Nettoie les champs dynamiques d'un profil. */
static void cleanup_profile(UserProfile *profile)
{
    if (profile == NULL)
    {
        return;
    }

    free(profile->name);
    free(profile->goal);
    free(profile->restrictions);
}

/* Construit le prompt français envoyé à Mistral. */
static char *build_prompt_from_profile(const UserProfile *profile)
{
    const char *goal = profile->goal != NULL ? profile->goal : "balance";
    const char *restrictions = profile->restrictions != NULL ? profile->restrictions : "Aucune";
    size_t size = (size_t)snprintf(NULL, 0,
        "Tu es un nutritionniste expert. Génère un plan de repas sur 7 jours pour :\n\nAge: %d, Poids: %.2fkg, Taille: %.2fcm,\n\nObjectif: %s, Restrictions: %s, Budget: %.2f MAD/semaine.\n\nRéponds UNIQUEMENT en JSON valide :\n\n{\"days\":[{\"day\":1,\"breakfast\":{\"name\":\"\",\"calories\":0,\"proteins\":0,\"carbs\":0,\"fats\":0},\"lunch\":{...},\"dinner\":{...},\"snack\":{...},\"total_calories\":0,\"macros\":{\"proteins\":0,\"carbs\":0,\"fats\":0}}]}",
        profile->age,
        profile->weight,
        profile->height,
        goal,
        restrictions,
        profile->budget);
    char *prompt = (char *)malloc(size + 1);

    if (prompt == NULL)
    {
        return NULL;
    }

    snprintf(prompt, size + 1,
        "Tu es un nutritionniste expert. Génère un plan de repas sur 7 jours pour :\n\nAge: %d, Poids: %.2fkg, Taille: %.2fcm,\n\nObjectif: %s, Restrictions: %s, Budget: %.2f MAD/semaine.\n\nRéponds UNIQUEMENT en JSON valide :\n\n{\"days\":[{\"day\":1,\"breakfast\":{\"name\":\"\",\"calories\":0,\"proteins\":0,\"carbs\":0,\"fats\":0},\"lunch\":{...},\"dinner\":{...},\"snack\":{...},\"total_calories\":0,\"macros\":{\"proteins\":0,\"carbs\":0,\"fats\":0}}]}",
        profile->age,
        profile->weight,
        profile->height,
        goal,
        restrictions,
        profile->budget);
    return prompt;
}

/* Vérifie si un texte contient une sous-chaîne sans sensibilité à la casse. */
static int contains_ignore_case(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0')
    {
        return 0;
    }

    for (const char *cursor = text; *cursor != '\0'; ++cursor)
    {
        const char *left = cursor;
        const char *right = needle;

        while (*left != '\0' && *right != '\0' && tolower((unsigned char)*left) == tolower((unsigned char)*right))
        {
            ++left;
            ++right;
        }

        if (*right == '\0')
        {
            return 1;
        }
    }

    return 0;
}

/* Devine la catégorie d'un item de liste de courses. */
static const char *guess_category(const char *name)
{
    if (name == NULL)
    {
        return "other";
    }

    if (contains_ignore_case(name, "chicken") || contains_ignore_case(name, "beef") || contains_ignore_case(name, "fish") || contains_ignore_case(name, "egg") || contains_ignore_case(name, "tofu"))
    {
        return "proteins";
    }

    if (contains_ignore_case(name, "rice") || contains_ignore_case(name, "pasta") || contains_ignore_case(name, "bread") || contains_ignore_case(name, "oats") || contains_ignore_case(name, "potato"))
    {
        return "carbs";
    }

    if (contains_ignore_case(name, "salad") || contains_ignore_case(name, "vegetable") || contains_ignore_case(name, "broccoli") || contains_ignore_case(name, "carrot") || contains_ignore_case(name, "spinach"))
    {
        return "vegetables";
    }

    if (contains_ignore_case(name, "milk") || contains_ignore_case(name, "yogurt") || contains_ignore_case(name, "fromage"))
    {
        return "dairy";
    }

    if (contains_ignore_case(name, "snack") || contains_ignore_case(name, "fruit") || contains_ignore_case(name, "nuts"))
    {
        return "snacks";
    }

    return "other";
}

/* Ajoute une chaîne unique à un tableau JSON. */
static void add_unique_string(cJSON *array, const char *value)
{
    int count = cJSON_GetArraySize(array);

    for (int index = 0; index < count; ++index)
    {
        cJSON *existing = cJSON_GetArrayItem(array, index);
        if (cJSON_IsString(existing) && existing->valuestring != NULL && strcmp(existing->valuestring, value) == 0)
        {
            return;
        }
    }

    cJSON_AddItemToArray(array, cJSON_CreateString(value));
}

/* Calcule un score moyen pour un plan complet. */
static float compute_plan_score(cJSON *plan_object, float target_daily_calories)
{
    cJSON *days = cJSON_GetObjectItemCaseSensitive(plan_object, "days");
    cJSON *day = NULL;
    int count = 0;
    float total_score = 0.0f;

    if (!cJSON_IsArray(days) || target_daily_calories <= 0.0f)
    {
        return 0.0f;
    }

    cJSON_ArrayForEach(day, days)
    {
        cJSON *macros = cJSON_GetObjectItemCaseSensitive(day, "macros");
        cJSON *protein = cJSON_GetObjectItemCaseSensitive(macros, "proteins");
        cJSON *carbs = cJSON_GetObjectItemCaseSensitive(macros, "carbs");
        cJSON *fats = cJSON_GetObjectItemCaseSensitive(macros, "fats");

        if (cJSON_IsNumber(protein) && cJSON_IsNumber(carbs) && cJSON_IsNumber(fats))
        {
            total_score += (float)score_meal((float)protein->valuedouble, (float)carbs->valuedouble, (float)fats->valuedouble, target_daily_calories);
            count++;
        }
    }

    if (count == 0)
    {
        return 0.0f;
    }

    return total_score / (float)count;
}

/* Génère une liste de courses simple depuis un plan de repas. */
static cJSON *build_shopping_list(const char *plan_json)
{
    const char *category_names[] = {"proteins", "carbs", "vegetables", "dairy", "snacks", "other"};
    const char *meal_names[] = {"breakfast", "lunch", "dinner", "snack"};
    cJSON *root = cJSON_Parse(plan_json);
    cJSON *shopping = cJSON_CreateObject();
    cJSON *categories = cJSON_CreateArray();
    cJSON *day = NULL;

    if (root == NULL || shopping == NULL || categories == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(shopping);
        cJSON_Delete(categories);
        return NULL;
    }

    for (size_t index = 0; index < sizeof(category_names) / sizeof(category_names[0]); ++index)
    {
        cJSON *category = cJSON_CreateObject();
        cJSON_AddStringToObject(category, "name", category_names[index]);
        cJSON_AddItemToObject(category, "items", cJSON_CreateArray());
        cJSON_AddItemToArray(categories, category);
    }

    cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
    if (cJSON_IsArray(days))
    {
        cJSON_ArrayForEach(day, days)
        {
            for (size_t index = 0; index < sizeof(meal_names) / sizeof(meal_names[0]); ++index)
            {
                cJSON *meal = cJSON_GetObjectItemCaseSensitive(day, meal_names[index]);
                cJSON *name = NULL;
                const char *meal_name;
                const char *category_name;

                if (meal == NULL)
                {
                    continue;
                }

                name = cJSON_GetObjectItemCaseSensitive(meal, "name");
                meal_name = cJSON_IsString(name) ? name->valuestring : NULL;
                if (meal_name == NULL || meal_name[0] == '\0')
                {
                    continue;
                }

                category_name = guess_category(meal_name);
                for (int category_index = 0; category_index < cJSON_GetArraySize(categories); ++category_index)
                {
                    cJSON *category = cJSON_GetArrayItem(categories, category_index);
                    cJSON *label = cJSON_GetObjectItemCaseSensitive(category, "name");

                    if (cJSON_IsString(label) && strcmp(label->valuestring, category_name) == 0)
                    {
                        cJSON *items = cJSON_GetObjectItemCaseSensitive(category, "items");
                        add_unique_string(items, meal_name);
                        break;
                    }
                }
            }
        }
    }

    cJSON_AddItemToObject(shopping, "categories", categories);
    cJSON_Delete(root);
    return shopping;
}

/* Lit un profil utilisateur depuis le corps JSON de la requête. */
static int handle_profile_post(struct MHD_Connection *connection, const char *body)
{
    cJSON *root = cJSON_Parse(body);
    cJSON *name_item;
    cJSON *age_item;
    cJSON *weight_item;
    cJSON *height_item;
    cJSON *goal_item;
    cJSON *budget_item;
    UserProfile profile = {0};
    char *json_text = NULL;
    int saved_id;

    if (root == NULL)
    {
        return queue_error(connection, MHD_HTTP_BAD_REQUEST, "JSON invalide");
    }

    name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    age_item = cJSON_GetObjectItemCaseSensitive(root, "age");
    weight_item = cJSON_GetObjectItemCaseSensitive(root, "weight");
    height_item = cJSON_GetObjectItemCaseSensitive(root, "height");
    goal_item = cJSON_GetObjectItemCaseSensitive(root, "goal");
    budget_item = cJSON_GetObjectItemCaseSensitive(root, "budget");

    profile.name = duplicate_json_string(root, "name");
    profile.goal = duplicate_json_string(root, "goal");
    profile.restrictions = join_restrictions(cJSON_GetObjectItemCaseSensitive(root, "restrictions"));
    profile.age = cJSON_IsNumber(age_item) ? age_item->valueint : 0;
    profile.weight = cJSON_IsNumber(weight_item) ? (float)weight_item->valuedouble : 0.0f;
    profile.height = cJSON_IsNumber(height_item) ? (float)height_item->valuedouble : 0.0f;
    profile.budget = cJSON_IsNumber(budget_item) ? (float)budget_item->valuedouble : 0.0f;

    if (!cJSON_IsString(name_item) || profile.name == NULL || profile.name[0] == '\0' || !cJSON_IsNumber(age_item) || !cJSON_IsNumber(weight_item) || !cJSON_IsNumber(height_item) || !cJSON_IsString(goal_item))
    {
        cleanup_profile(&profile);
        cJSON_Delete(root);
        return queue_error(connection, MHD_HTTP_BAD_REQUEST, "Champs de profil manquants");
    }

    saved_id = db_save_profile(&profile);
    if (saved_id < 0)
    {
        cleanup_profile(&profile);
        cJSON_Delete(root);
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible d'enregistrer le profil");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "user_id", saved_id);
    json_text = wrap_payload(data, "ok", NULL);

    cleanup_profile(&profile);
    cJSON_Delete(root);

    if (json_text == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de sérialisation");
    }

    saved_id = queue_json(connection, MHD_HTTP_OK, json_text);
    free(json_text);
    return saved_id;
}

/* Calcule un score nutritionnel à partir des macros passées en requête. */
static int handle_score_get(struct MHD_Connection *connection)
{
    int user_id = query_int(connection, "user_id", 0);
    float proteins = query_float(connection, "proteins", 0.0f);
    float carbs = query_float(connection, "carbs", 0.0f);
    float fats = query_float(connection, "fats", 0.0f);
    float target_calories = query_float(connection, "target_calories", 0.0f);
    UserProfile *profile = NULL;
    cJSON *data = cJSON_CreateObject();
    char *json_text;
    int score;

    if (target_calories <= 0.0f)
    {
        profile = db_get_profile(user_id);
        if (profile != NULL)
        {
            float tmb = calculate_tmb(profile->age, profile->weight, profile->height, "m");
            target_calories = calculate_daily_calories(tmb, profile->goal);
        }
    }

    score = score_meal(proteins, carbs, fats, target_calories);
    if (profile != NULL)
    {
        free_user_profile(profile);
    }

    cJSON_AddNumberToObject(data, "score", score);
    json_text = wrap_payload(data, "ok", NULL);
    if (json_text == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de calcul du score");
    }

    score = queue_json(connection, MHD_HTTP_OK, json_text);
    free(json_text);
    return score;
}

/* Génère un plan via Mistral, le stocke puis le renvoie au frontend. */
static int handle_plan_get(struct MHD_Connection *connection)
{
    int user_id = query_int(connection, "user_id", 0);
    UserProfile *profile = db_get_profile(user_id);
    char *prompt = NULL;
    char *api_key = getenv("MISTRAL_API_KEY");
    char *mistral_raw = NULL;
    char *assistant_content = NULL;
    char *plan_json_text = NULL;
    const char *plan_storage_json = NULL;
    cJSON *plan_object = NULL;
    cJSON *data = NULL;
    cJSON *days = NULL;
    char *json_text = NULL;
    float target_calories;
    float plan_score;
    int saved_plan_id;
    int synced = 0;

    if (profile == NULL)
    {
        return queue_error(connection, MHD_HTTP_NOT_FOUND, "Profil introuvable");
    }

    prompt = build_prompt_from_profile(profile);
    if (prompt == NULL)
    {
        free_user_profile(profile);
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible de construire l'invite");
    }

    mistral_raw = call_mistral_api(prompt, api_key);
    if (mistral_raw == NULL)
    {
        free(prompt);
        free_user_profile(profile);
        return queue_error(connection, MHD_HTTP_BAD_GATEWAY, "Erreur lors de l'appel à Mistral");
    }

    assistant_content = extract_mistral_content(mistral_raw);
    if (assistant_content == NULL)
    {
        assistant_content = strdup(mistral_raw);
    }

    plan_json_text = extract_json_fragment(assistant_content);
    plan_storage_json = plan_json_text != NULL ? plan_json_text : assistant_content;
    plan_object = cJSON_Parse(plan_storage_json);
    if (plan_object == NULL)
    {
        free(prompt);
        free(mistral_raw);
        free(assistant_content);
        free(plan_json_text);
        free_user_profile(profile);
        return queue_error(connection, MHD_HTTP_BAD_GATEWAY, "La réponse du modèle n'est pas un JSON valide");
    }

    target_calories = calculate_daily_calories(calculate_tmb(profile->age, profile->weight, profile->height, "m"), profile->goal);
    plan_score = compute_plan_score(plan_object, target_calories);

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(plan_object);
        free(prompt);
        free(mistral_raw);
        free(assistant_content);
        free(plan_json_text);
        free_user_profile(profile);
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible de sérialiser le plan");
    }

    cJSON_AddNumberToObject(data, "score", plan_score);
    cJSON_AddItemToObject(data, "plan", plan_object);

    json_text = wrap_payload(data, "ok", NULL);
    if (json_text == NULL)
    {
        free(prompt);
        free(mistral_raw);
        free(assistant_content);
        free(plan_json_text);
        free_user_profile(profile);
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de sérialisation du plan");
    }

    saved_plan_id = db_save_plan(profile->id, (char *)plan_storage_json, plan_score);
    if (saved_plan_id > 0)
    {
        char *supabase_url = getenv("SUPABASE_URL");
        char *supabase_key = getenv("SUPABASE_KEY");

        if (supabase_url != NULL && supabase_key != NULL && sync_plan_to_supabase((char *)plan_storage_json, supabase_url, supabase_key) == 0)
        {
            db_sync_flag(saved_plan_id);
            synced = 1;
        }
    }

    free(prompt);
    free(mistral_raw);
    free(assistant_content);
    free(plan_json_text);
    free_user_profile(profile);

    days = cJSON_GetObjectItemCaseSensitive(plan_object, "days");
    (void)days;

    {
        cJSON *response_data = cJSON_CreateObject();
        cJSON *plan_copy = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(data, "plan"), 1);
        if (response_data == NULL || plan_copy == NULL)
        {
            cJSON_Delete(response_data);
            free(json_text);
            return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de duplication du plan");
        }

        cJSON_AddNumberToObject(response_data, "score", plan_score);
        cJSON_AddNumberToObject(response_data, "saved_plan_id", saved_plan_id);
        cJSON_AddNumberToObject(response_data, "synced_supabase", synced);
        cJSON_AddItemToObject(response_data, "plan", plan_copy);

        free(json_text);
        json_text = wrap_payload(response_data, "ok", NULL);
    }

    if (json_text == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de sérialisation du plan");
    }

    saved_plan_id = queue_json(connection, MHD_HTTP_OK, json_text);
    free(json_text);
    return saved_plan_id;
}

/* Retourne l'historique des plans stockés dans SQLite. */
static int handle_history_get(struct MHD_Connection *connection)
{
    int user_id = query_int(connection, "user_id", 0);
    char *history_json = db_get_history(user_id);
    cJSON *history_object = NULL;
    cJSON *data = NULL;
    char *json_text = NULL;

    if (history_json == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible de récupérer l'historique");
    }

    history_object = cJSON_Parse(history_json);
    free(history_json);
    if (history_object == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Historique JSON invalide");
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(history_object);
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible de sérialiser l'historique");
    }

    cJSON_AddItemToObject(data, "history", history_object);
    json_text = wrap_payload(data, "ok", NULL);
    if (json_text == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de sérialisation de l'historique");
    }

    user_id = queue_json(connection, MHD_HTTP_OK, json_text);
    free(json_text);
    return user_id;
}

/* Construit une liste de courses depuis le dernier plan disponible. */
static int handle_shopping_list_get(struct MHD_Connection *connection)
{
    int user_id = query_int(connection, "user_id", 0);
    char *latest_plan_json = db_get_latest_plan_json(user_id);
    cJSON *shopping_list = NULL;
    cJSON *data = NULL;
    char *json_text = NULL;

    if (latest_plan_json == NULL)
    {
        return queue_error(connection, MHD_HTTP_NOT_FOUND, "Aucun plan disponible");
    }

    shopping_list = build_shopping_list(latest_plan_json);
    free(latest_plan_json);
    if (shopping_list == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible de générer la liste de courses");
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(shopping_list);
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Impossible de sérialiser la liste de courses");
    }

    cJSON_AddItemToObject(data, "shopping_list", shopping_list);
    json_text = wrap_payload(data, "ok", NULL);
    if (json_text == NULL)
    {
        return queue_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Erreur de sérialisation de la liste de courses");
    }

    user_id = queue_json(connection, MHD_HTTP_OK, json_text);
    free(json_text);
    return user_id;
}

/* Répond aux requêtes OPTIONS pour le CORS. */
static int handle_options(struct MHD_Connection *connection)
{
    return queue_json(connection, MHD_HTTP_NO_CONTENT, "");
}

/* Sélectionne la route HTTP adéquate. */
static int route_request(struct MHD_Connection *connection, const char *method, const char *url, const char *body)
{
    if (strcmp(method, MHD_HTTP_METHOD_OPTIONS) == 0)
    {
        return handle_options(connection);
    }

    if (strcmp(method, MHD_HTTP_METHOD_POST) == 0 && strcmp(url, "/api/profile") == 0)
    {
        return handle_profile_post(connection, body);
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 && strcmp(url, "/api/plan") == 0)
    {
        return handle_plan_get(connection);
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 && strcmp(url, "/api/score") == 0)
    {
        return handle_score_get(connection);
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 && strcmp(url, "/api/history") == 0)
    {
        return handle_history_get(connection);
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 && strcmp(url, "/api/shopping-list") == 0)
    {
        return handle_shopping_list_get(connection);
    }

    return queue_error(connection, MHD_HTTP_NOT_FOUND, "Route introuvable");
}

/* Ajoute un segment de corps POST au buffer de la requête. */
static void append_upload_data(RequestContext *context, const char *upload_data, size_t upload_size)
{
    char *new_body;

    if (context->body_size + upload_size + 1 > MAX_BODY_SIZE)
    {
        return;
    }

    new_body = (char *)realloc(context->body, context->body_size + upload_size + 1);
    if (new_body == NULL)
    {
        return;
    }

    context->body = new_body;
    memcpy(context->body + context->body_size, upload_data, upload_size);
    context->body_size += upload_size;
    context->body[context->body_size] = '\0';
}

/* Gère le cycle de vie des requêtes HTTP libmicrohttpd. */
static int access_handler(void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    RequestContext *context = (RequestContext *)(*con_cls);
    int result;

    (void)cls;
    (void)version;

    if (context == NULL)
    {
        context = (RequestContext *)calloc(1, sizeof(RequestContext));
        if (context == NULL)
        {
            return MHD_NO;
        }

        *con_cls = context;
        return MHD_YES;
    }

    if (strcmp(method, MHD_HTTP_METHOD_POST) == 0 && *upload_data_size > 0)
    {
        append_upload_data(context, upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    result = route_request(connection, method, url, context->body != NULL ? context->body : "{}");
    free_request_context(context);
    *con_cls = NULL;
    return result;
}

/* Point d'entrée du serveur HTTP. */
int main(void)
{
    struct MHD_Daemon *daemon;
    const char *db_path = "project-groupeX-ai-food-planner.sqlite3";

    if (db_init(db_path) != 0)
    {
        fprintf(stderr, "Impossible d'initialiser SQLite\n");
        return EXIT_FAILURE;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        fprintf(stderr, "Impossible d'initialiser libcurl\n");
        db_close();
        return EXIT_FAILURE;
    }

    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, SERVER_PORT, NULL, NULL, &access_handler, NULL, MHD_OPTION_END);
    if (daemon == NULL)
    {
        fprintf(stderr, "Impossible de démarrer le serveur HTTP\n");
        curl_global_cleanup();
        db_close();
        return EXIT_FAILURE;
    }

    printf("AI Food Planner écoute sur http://localhost:%d\n", SERVER_PORT);
    printf("Appuyez sur Entrée pour arrêter le serveur.\n");
    (void)getchar();

    MHD_stop_daemon(daemon);
    curl_global_cleanup();
    db_close();
    return EXIT_SUCCESS;
}
